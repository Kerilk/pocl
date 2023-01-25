/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2021 Aksel Alpay and contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "SubCfgFormation.h"

#include "Barrier.h"
#include "LLVMUtils.h"
#include "VariableUniformityAnalysis.h"
#include "Workgroup.h"
#include "WorkitemHandlerChooser.h"
#include "pocl_llvm_api.h"

#include <cstddef>
#include <llvm/ADT/Twine.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Regex.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>

// #define DEBUG_SUBCFG_FORMATION

namespace pocl {
static constexpr size_t NumArrayElements = 1024;
static constexpr size_t DefaultAlignment = 64;
static constexpr const char LoopStateMD[] = "poclLoopState";

struct MDKind {
  static constexpr const char Arrayified[] = "pocl.arrayified";
  static constexpr const char InnerLoop[] = "pocl.loop.inner";
  static constexpr const char WorkItemLoop[] = "pocl.loop.workitem";
};

static constexpr const char LocalIdGlobalNameX[] = "_local_id_x";
static constexpr const char LocalIdGlobalNameY[] = "_local_id_y";
static constexpr const char LocalIdGlobalNameZ[] = "_local_id_z";
static const std::array<const char *, 3> LocalIdGlobalNames{
    LocalIdGlobalNameX, LocalIdGlobalNameY, LocalIdGlobalNameZ};

} // namespace pocl

namespace {
using namespace pocl;

static const std::array<char, 3> DimName{'x', 'y', 'z'};

llvm::Loop *updateDtAndLi(llvm::LoopInfo &LI, llvm::DominatorTree &DT,
                          const llvm::BasicBlock *B, llvm::Function &F) {
  DT.reset();
  DT.recalculate(F);
  LI.releaseMemory();
  LI.analyze(DT);
  return LI.getLoopFor(B);
}

template <class UserType, class Func>
bool anyOfUsers(llvm::Value *V, Func &&L) {
  for (auto *U : V->users())
    if (UserType *UT = llvm::dyn_cast<UserType>(U))
      if (L(UT))
        return true;
  return false;
}

template <class UserType, class Func>
bool noneOfUsers(llvm::Value *V, Func &&L) {
  return !anyOfUsers<UserType>(V, std::forward<Func>(L));
}

template <class UserType, class Func>
bool allOfUsers(llvm::Value *V, Func &&L) {
  return !anyOfUsers<UserType>(
      V, [L = std::forward<Func>(L)](UserType *UT) { return !L(UT); });
}

/// Arrayification of work item private values
void arrayifyAllocas(llvm::BasicBlock *EntryBlock, llvm::Loop &L,
                     llvm::Value *Idx, const llvm::DominatorTree &DT);
llvm::AllocaInst *arrayifyValue(llvm::Instruction *IPAllocas,
                                llvm::Value *ToArrayify,
                                llvm::Instruction *InsertionPoint,
                                llvm::Value *Idx,
                                size_t NumValues = NumArrayElements,
                                llvm::MDTuple *MDAlloca = nullptr);
llvm::AllocaInst *arrayifyInstruction(llvm::Instruction *IPAllocas,
                                      llvm::Instruction *ToArrayify,
                                      llvm::Value *Idx,
                                      size_t NumValues = NumArrayElements,
                                      llvm::MDTuple *MDAlloca = nullptr);
llvm::LoadInst *loadFromAlloca(llvm::AllocaInst *Alloca, llvm::Value *Idx,
                               llvm::Instruction *InsertBefore,
                               const llvm::Twine &NamePrefix = "");

llvm::AllocaInst *getLoopStateAllocaForLoad(llvm::LoadInst &LInst);

void arrayifyAllocas(llvm::BasicBlock *EntryBlock, llvm::Loop &L,
                     llvm::Value *Idx, const llvm::DominatorTree &DT) {
  assert(Idx && "Valid WI-Index required");

  auto *MDAlloca = llvm::MDNode::get(
      EntryBlock->getContext(),
      {llvm::MDString::get(EntryBlock->getContext(), LoopStateMD)});

  auto &LoopBlocks = L.getBlocksSet();
  llvm::SmallVector<llvm::AllocaInst *, 8> WL;
  for (auto &I : *EntryBlock) {
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
      if (llvm::MDNode *MD = Alloca->getMetadata(pocl::MDKind::Arrayified))
        continue; // already arrayificated
      if (!std::all_of(Alloca->user_begin(), Alloca->user_end(),
                       [&LoopBlocks](llvm::User *User) {
                         auto *Inst = llvm::dyn_cast<llvm::Instruction>(User);
                         return Inst && LoopBlocks.contains(Inst->getParent());
                       }))
        continue;
      WL.push_back(Alloca);
    }
  }

  for (auto *I : WL) {
    llvm::IRBuilder AllocaBuilder{I};
    llvm::Type *T = I->getAllocatedType();
    if (auto *ArrSizeC = llvm::dyn_cast<llvm::ConstantInt>(I->getArraySize())) {
      auto ArrSize = ArrSizeC->getLimitedValue();
      if (ArrSize > 1) {
        T = llvm::ArrayType::get(T, ArrSize);
        llvm::errs() << "Caution, alloca was array\n";
      }
    }

    auto *Alloca = AllocaBuilder.CreateAlloca(
        T, AllocaBuilder.getInt32(pocl::NumArrayElements),
        I->getName() + "_alloca");
    Alloca->setAlignment(llvm::Align{pocl::DefaultAlignment});
    Alloca->setMetadata(pocl::MDKind::Arrayified, MDAlloca);

    llvm::Instruction *GepIp = nullptr;
    for (auto *U : I->users()) {
      if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
        if (!GepIp || DT.dominates(UI, GepIp))
          GepIp = UI;
      }
    }
    if (GepIp) {
      llvm::IRBuilder LoadBuilder{GepIp};
      auto *GEP =
          llvm::cast<llvm::GetElementPtrInst>(LoadBuilder.CreateInBoundsGEP(
              Alloca->getAllocatedType(), Alloca, Idx, I->getName() + "_gep"));
      GEP->setMetadata(pocl::MDKind::Arrayified, MDAlloca);

      I->replaceAllUsesWith(GEP);
      I->eraseFromParent();
    }
  }
}

llvm::AllocaInst *arrayifyValue(llvm::Instruction *IPAllocas,
                                llvm::Value *ToArrayify,
                                llvm::Instruction *InsertionPoint,
                                llvm::Value *Idx, size_t NumElements,
                                llvm::MDTuple *MDAlloca) {
  assert(Idx && "Valid WI-Index required");

  if (!MDAlloca)
    MDAlloca = llvm::MDNode::get(
        IPAllocas->getContext(),
        {llvm::MDString::get(IPAllocas->getContext(), LoopStateMD)});

  auto *T = ToArrayify->getType();
  llvm::IRBuilder AllocaBuilder{IPAllocas};
  auto *Alloca = AllocaBuilder.CreateAlloca(
      T, NumElements == 1 ? nullptr : AllocaBuilder.getInt32(NumElements),
      ToArrayify->getName() + "_alloca");
  if (NumElements > 1)
    Alloca->setAlignment(llvm::Align{pocl::DefaultAlignment});
  Alloca->setMetadata(pocl::MDKind::Arrayified, MDAlloca);

  const llvm::DataLayout &Layout =
      InsertionPoint->getParent()->getParent()->getParent()->getDataLayout();

  llvm::IRBuilder WriteBuilder{InsertionPoint};
  llvm::Value *StoreTarget = Alloca;
  if (NumElements != 1) {
    auto *GEP = llvm::cast<llvm::GetElementPtrInst>(
        WriteBuilder.CreateInBoundsGEP(Alloca->getAllocatedType(), Alloca, Idx,
                                       ToArrayify->getName() + "_gep"));
    GEP->setMetadata(pocl::MDKind::Arrayified, MDAlloca);
    StoreTarget = GEP;
  }
  WriteBuilder.CreateStore(ToArrayify, StoreTarget);
  return Alloca;
}

llvm::AllocaInst *arrayifyInstruction(llvm::Instruction *IPAllocas,
                                      llvm::Instruction *ToArrayify,
                                      llvm::Value *Idx, size_t NumElements,
                                      llvm::MDTuple *MDAlloca) {
  llvm::Instruction *InsertionPoint = &*(++ToArrayify->getIterator());
  if (llvm::isa<llvm::PHINode>(ToArrayify))
    InsertionPoint = ToArrayify->getParent()->getFirstNonPHI();

  return arrayifyValue(IPAllocas, ToArrayify, InsertionPoint, Idx, NumElements,
                       MDAlloca);
}

llvm::LoadInst *loadFromAlloca(llvm::AllocaInst *Alloca, llvm::Value *Idx,
                               llvm::Instruction *InsertBefore,
                               const llvm::Twine &NamePrefix) {
  assert(Idx && "Valid WI-Index required");
  auto *MDAlloca = Alloca->getMetadata(pocl::MDKind::Arrayified);

  llvm::IRBuilder LoadBuilder{InsertBefore};
  llvm::Value *LoadFrom = Alloca;
  if (Alloca->isArrayAllocation()) {
    auto *GEP =
        llvm::cast<llvm::GetElementPtrInst>(LoadBuilder.CreateInBoundsGEP(
            Alloca->getAllocatedType(), Alloca, Idx, NamePrefix + "_lgep"));
    GEP->setMetadata(pocl::MDKind::Arrayified, MDAlloca);
    LoadFrom = GEP;
  }
  auto *Load = LoadBuilder.CreateLoad(Alloca->getAllocatedType(), LoadFrom,
                                      NamePrefix + "_load");
  return Load;
}

llvm::AllocaInst *getLoopStateAllocaForLoad(llvm::LoadInst &LInst) {
  llvm::AllocaInst *Alloca = nullptr;
  if (auto *GEPI =
          llvm::dyn_cast<llvm::GetElementPtrInst>(LInst.getPointerOperand())) {
    Alloca = llvm::dyn_cast<llvm::AllocaInst>(GEPI->getPointerOperand());
  } else {
    Alloca = llvm::dyn_cast<llvm::AllocaInst>(LInst.getPointerOperand());
  }
  if (Alloca && Alloca->hasMetadata(pocl::MDKind::Arrayified))
    return Alloca;
  return nullptr;
}

// gets the load inside F from the global variable called VarName
llvm::Value *getLoadForGlobalVariable(llvm::Function &F,
                                      llvm::StringRef VarName) {
  auto *GV = F.getParent()->getGlobalVariable(VarName);
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *LoadI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        if (LoadI->getPointerOperand() == GV)
          return &I;
      }
    }
  }
  llvm::IRBuilder Builder{F.getEntryBlock().getTerminator()};
  return Builder.CreateLoad(
      F.getParent()->getDataLayout().getLargestLegalIntType(F.getContext()),
      GV);
}

// get the wg size values for the loop bounds
llvm::SmallVector<llvm::Value *, 3>
getLocalSizeValues(llvm::Function &F, llvm::ArrayRef<std::size_t> LocalSizes,
                   bool DynSizes, int Dim) {
  auto &DL = F.getParent()->getDataLayout();

  llvm::SmallVector<llvm::Value *, 3> LocalSize(Dim);
  for (int D = 0; D < Dim; ++D) {
    if (DynSizes)
      LocalSize[D] =
          getLoadForGlobalVariable(F, std::string{"_local_size_"} + DimName[D]);
    else
      LocalSize[D] = llvm::ConstantInt::get(
          DL.getLargestLegalIntType(F.getContext()), LocalSizes[D]);
  }

  return LocalSize;
}

// create the wi-loops around a kernel or subCFG, LastHeader input should be the
// load block, ContiguousIdx may be any identifyable value (load from undef)
void createLoopsAround(llvm::Function &F, llvm::BasicBlock *AfterBB,
                       const llvm::ArrayRef<llvm::Value *> &LocalSize,
                       int EntryId, llvm::ValueToValueMapTy &VMap,
                       llvm::SmallVector<llvm::BasicBlock *, 3> &Latches,
                       llvm::BasicBlock *&LastHeader,
                       llvm::Value *&ContiguousIdx) {
  const auto &DL = F.getParent()->getDataLayout();
  auto *LoadBB = LastHeader;
  llvm::IRBuilder Builder{LoadBB, LoadBB->getFirstInsertionPt()};

  const size_t Dim = LocalSize.size();

  // from innermost to outermost: create loops around the LastHeader and use
  // AfterBB as dummy exit to be replaced by the outer latch later
  llvm::SmallVector<llvm::PHINode *, 3> IndVars;
  for (int D = Dim - 1; D >= 0; --D) {
    const std::string Suffix =
        (llvm::Twine{DimName[D]} + ".subcfg." + llvm::Twine{EntryId}).str();

    auto *Header = llvm::BasicBlock::Create(
        LastHeader->getContext(), "header." + Suffix + "b",
        LastHeader->getParent(), LastHeader);

    Builder.SetInsertPoint(Header, Header->getFirstInsertionPt());

    auto *WIIndVar = Builder.CreatePHI(
        DL.getLargestLegalIntType(F.getContext()), 2, "indvar." + Suffix);
    WIIndVar->addIncoming(
        Builder.getIntN(DL.getLargestLegalIntTypeSizeInBits(), 0),
        &F.getEntryBlock());
    IndVars.push_back(WIIndVar);
    Builder.CreateBr(LastHeader);

    auto *Latch =
        llvm::BasicBlock::Create(F.getContext(), "latch." + Suffix + "b", &F);
    Builder.SetInsertPoint(Latch, Latch->getFirstInsertionPt());
    auto *IncIndVar = Builder.CreateAdd(
        WIIndVar, Builder.getIntN(DL.getLargestLegalIntTypeSizeInBits(), 1),
        "addInd." + Suffix, true, false);
    WIIndVar->addIncoming(IncIndVar, Latch);

    auto *LoopCond =
        Builder.CreateICmpULT(IncIndVar, LocalSize[D], "exit.cond." + Suffix);
    Builder.CreateCondBr(LoopCond, Header, AfterBB);
    Latches.push_back(Latch);
    LastHeader = Header;
  }

  std::reverse(Latches.begin(), Latches.end());
  std::reverse(IndVars.begin(), IndVars.end());

  for (size_t D = 1; D < Dim; ++D) {
    Latches[D]->getTerminator()->replaceSuccessorWith(AfterBB, Latches[D - 1]);
    IndVars[D]->replaceIncomingBlockWith(&F.getEntryBlock(),
                                         IndVars[D - 1]->getParent());
  }

  auto *MDWorkItemLoop = llvm::MDNode::get(
      F.getContext(),
      {llvm::MDString::get(F.getContext(), MDKind::WorkItemLoop)});
  auto *LoopID = llvm::makePostTransformationMetadata(F.getContext(), nullptr,
                                                      {}, {MDWorkItemLoop});
  Latches[Dim - 1]->getTerminator()->setMetadata("llvm.loop", LoopID);
  VMap[AfterBB] = Latches[Dim - 1];

  // add contiguous ind var calculation to load block
  Builder.SetInsertPoint(IndVars[Dim - 1]->getParent(),
                         ++IndVars[Dim - 1]->getIterator());
  llvm::Value *Idx = IndVars[0];
  for (size_t D = 1; D < Dim; ++D) {
    const std::string Suffix =
        (llvm::Twine{DimName[D]} + ".subcfg." + llvm::Twine{EntryId}).str();

    Idx = Builder.CreateMul(Idx, LocalSize[D], "idx.mul." + Suffix, true);
    Idx = Builder.CreateAdd(IndVars[D], Idx, "idx.add." + Suffix, true);

    VMap[getLoadForGlobalVariable(F, LocalIdGlobalNames[D])] = IndVars[D];
  }

  // todo: replace `ret` with branch to innermost latch

  VMap[getLoadForGlobalVariable(F, LocalIdGlobalNames[0])] = IndVars[0];

  VMap[ContiguousIdx] = Idx;
  ContiguousIdx = Idx;
}

class SubCFG {

  using BlockVector = llvm::SmallVector<llvm::BasicBlock *, 8>;
  BlockVector Blocks_;
  BlockVector NewBlocks_;
  size_t EntryId_;
  llvm::BasicBlock *EntryBarrier_;
  llvm::SmallDenseMap<llvm::BasicBlock *, size_t> ExitIds_;
  llvm::AllocaInst *LastBarrierIdStorage_;
  llvm::Value *ContIdx_;
  llvm::BasicBlock *EntryBB_;
  llvm::BasicBlock *ExitBB_;
  llvm::BasicBlock *LoadBB_;
  llvm::BasicBlock *PreHeader_;
  size_t Dim;

  //  void addBlock(llvm::BasicBlock *BB) { Blocks_.push_back(BB); }
  llvm::BasicBlock *createExitWithID(
      llvm::detail::DenseMapPair<llvm::BasicBlock *, unsigned long> BarrierPair,
      llvm::BasicBlock *After, llvm::BasicBlock *TargetBB);

  void loadMultiSubCfgValues(
      const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
          &InstAllocaMap,
      llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
          &BaseInstAllocaMap,
      llvm::DenseMap<llvm::Instruction *,
                     llvm::SmallVector<llvm::Instruction *, 8>>
          &ContInstReplicaMap,
      llvm::BasicBlock *UniformLoadBB, llvm::ValueToValueMapTy &VMap);
  void loadUniformAndRecalcContValues(
      llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
          &BaseInstAllocaMap,
      llvm::DenseMap<llvm::Instruction *,
                     llvm::SmallVector<llvm::Instruction *, 8>>
          &ContInstReplicaMap,
      llvm::BasicBlock *UniformLoadBB, llvm::ValueToValueMapTy &VMap);
  llvm::BasicBlock *createLoadBB(llvm::ValueToValueMapTy &VMap);
  llvm::BasicBlock *createUniformLoadBB(llvm::BasicBlock *OuterMostHeader);

public:
  SubCFG(llvm::BasicBlock *EntryBarrier, llvm::AllocaInst *LastBarrierIdStorage,
         const llvm::DenseMap<llvm::BasicBlock *, size_t> &BarrierIds,
         llvm::Value *IndVar, size_t Dim);

  SubCFG(const SubCFG &) = delete;
  SubCFG &operator=(const SubCFG &) = delete;

  SubCFG(SubCFG &&) = default;
  SubCFG &operator=(SubCFG &&) = default;

  BlockVector &getBlocks() noexcept { return Blocks_; }
  const BlockVector &getBlocks() const noexcept { return Blocks_; }

  BlockVector &getNewBlocks() noexcept { return NewBlocks_; }
  const BlockVector &getNewBlocks() const noexcept { return NewBlocks_; }

  size_t getEntryId() const noexcept { return EntryId_; }

  llvm::BasicBlock *getEntry() noexcept { return EntryBB_; }
  llvm::BasicBlock *getExit() noexcept { return ExitBB_; }
  llvm::BasicBlock *getLoadBB() noexcept { return LoadBB_; }
  llvm::Value *getContiguousIdx() noexcept { return ContIdx_; }

  void replicate(llvm::Function &F,
                 const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
                     &InstAllocaMap,
                 llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
                     &BaseInstAllocaMap,
                 llvm::DenseMap<llvm::Instruction *,
                                llvm::SmallVector<llvm::Instruction *, 8>>
                     &ContInstReplicaMap,
                 llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
                     &RemappedInstAllocaMap,
                 llvm::BasicBlock *AfterBB,
                 llvm::ArrayRef<llvm::Value *> LocalSize);

  void arrayifyMultiSubCfgValues(
      llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &InstAllocaMap,
      llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
          &BaseInstAllocaMap,
      llvm::DenseMap<llvm::Instruction *,
                     llvm::SmallVector<llvm::Instruction *, 8>>
          &ContInstReplicaMap,
      llvm::ArrayRef<SubCFG> SubCFGs, llvm::Instruction *AllocaIP,
      size_t ReqdArrayElements, pocl::VariableUniformityAnalysis &VecInfo);
  void fixSingleSubCfgValues(
      llvm::DominatorTree &DT,
      const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
          &RemappedInstAllocaMap,
      std::size_t ReqdArrayElements, pocl::VariableUniformityAnalysis &VecInfo);

  void print() const;
  void removeDeadPhiBlocks(
      llvm::SmallVector<llvm::BasicBlock *, 8> &BlocksToRemap) const;
  llvm::SmallVector<llvm::Instruction *, 16> topoSortInstructions(
      const llvm::SmallPtrSet<llvm::Instruction *, 16> &UniquifyInsts) const;
};

// create new exiting block writing the exit's id to LastBarrierIdStorage_
llvm::BasicBlock *SubCFG::createExitWithID(
    llvm::detail::DenseMapPair<llvm::BasicBlock *, size_t> BarrierPair,
    llvm::BasicBlock *After, llvm::BasicBlock *TargetBB) {
  llvm::errs() << "Create new exit with ID: " << BarrierPair.second << " at "
               << After->getName() << "\n";

  auto *Exit = llvm::BasicBlock::Create(
      After->getContext(),
      After->getName() + ".subcfg.exit" + llvm::Twine{BarrierPair.second} + "b",
      After->getParent(), TargetBB);

  auto &DL = Exit->getParent()->getParent()->getDataLayout();
  llvm::IRBuilder Builder{Exit, Exit->getFirstInsertionPt()};
  Builder.CreateStore(Builder.getIntN(DL.getLargestLegalIntTypeSizeInBits(),
                                      BarrierPair.second),
                      LastBarrierIdStorage_);
  Builder.CreateBr(TargetBB);

  After->getTerminator()->replaceSuccessorWith(BarrierPair.first, Exit);
  return Exit;
}

// identify a new SubCFG using DFS starting at EntryBarrier
SubCFG::SubCFG(llvm::BasicBlock *EntryBarrier,
               llvm::AllocaInst *LastBarrierIdStorage,
               const llvm::DenseMap<llvm::BasicBlock *, size_t> &BarrierIds,
               llvm::Value *IndVar, size_t Dim)
    : LastBarrierIdStorage_(LastBarrierIdStorage),
      EntryId_(BarrierIds.lookup(EntryBarrier)), EntryBarrier_(EntryBarrier),
      EntryBB_(EntryBarrier->getSingleSuccessor()), LoadBB_(nullptr),
      ContIdx_(IndVar), PreHeader_(nullptr), Dim(Dim) {
  assert(ContIdx_ && "Must have found __hipsycl_local_id_{x,y,z}");

  llvm::SmallVector<llvm::BasicBlock *, 4> WL{EntryBarrier};
  while (!WL.empty()) {
    auto *BB = WL.pop_back_val();

    llvm::SmallVector<llvm::BasicBlock *, 2> Succs{llvm::succ_begin(BB),
                                                   llvm::succ_end(BB)};
    for (auto *Succ : Succs) {
      if (std::find(Blocks_.begin(), Blocks_.end(), Succ) != Blocks_.end())
        continue;

      if (!Barrier::hasOnlyBarrier(Succ)) {
        WL.push_back(Succ);
        Blocks_.push_back(Succ);
      } else {
        size_t BId = BarrierIds.lookup(Succ);
        assert(BId != 0 && "Exit barrier block not found in map");
        ExitIds_.insert({Succ, BId});
      }
    }
  }
}

void SubCFG::print() const {
#ifdef DEBUG_SUBCFG_FORMATION
  llvm::errs() << "SubCFG entry barrier: " << EntryId_ << "\n";
  llvm::errs() << "SubCFG block names: ";
  for (auto *BB : Blocks_) {
    llvm::errs() << BB->getName() << ", ";
  }
  llvm::errs() << "\n";
  llvm::errs() << "SubCFG exits: ";
  for (auto ExitIt : ExitIds_) {
    llvm::errs() << ExitIt.first->getName() << " (" << ExitIt.second << "), ";
  }
  llvm::errs() << "\n";
  llvm::errs() << "SubCFG new block names: ";
  for (auto *BB : NewBlocks_) {
    llvm::errs() << BB->getName() << ", ";
  }
  llvm::errs() << "\n";
#endif
}

void addRemappedDenseMapKeys(
    const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
        &OrgInstAllocaMap,
    const llvm::ValueToValueMapTy &VMap,
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &NewInstAllocaMap) {
  for (auto &InstAllocaPair : OrgInstAllocaMap) {
    if (auto *NewInst = llvm::dyn_cast_or_null<llvm::Instruction>(
            VMap.lookup(InstAllocaPair.first)))
      NewInstAllocaMap.insert({NewInst, InstAllocaPair.second});
  }
}

// clone all BBs of the subcfg, create wi-loop structure around and fixup values
void SubCFG::replicate(
    llvm::Function &F,
    const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
        &InstAllocaMap,
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &BaseInstAllocaMap,
    llvm::DenseMap<llvm::Instruction *,
                   llvm::SmallVector<llvm::Instruction *, 8>>
        &ContInstReplicaMap,
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
        &RemappedInstAllocaMap,
    llvm::BasicBlock *AfterBB, llvm::ArrayRef<llvm::Value *> LocalSize) {
  auto &DL = F.getParent()->getDataLayout();
  llvm::ValueToValueMapTy VMap;

  // clone blocks
  for (auto *BB : Blocks_) {
    auto *NewBB = llvm::CloneBasicBlock(
        BB, VMap, ".subcfg." + llvm::Twine{EntryId_} + "b", &F);
    VMap[BB] = NewBB;
    NewBlocks_.push_back(NewBB);
    for (auto *Succ : llvm::successors(BB)) {
      if (auto ExitIt = ExitIds_.find(Succ); ExitIt != ExitIds_.end()) {
        NewBlocks_.push_back(createExitWithID(*ExitIt, NewBB, AfterBB));
      }
    }
  }

  LoadBB_ = createLoadBB(VMap);

  VMap[EntryBarrier_] = LoadBB_;

  llvm::SmallVector<llvm::BasicBlock *, 3> Latches;
  llvm::BasicBlock *LastHeader = LoadBB_;
  llvm::Value *Idx = ContIdx_;

  createLoopsAround(F, AfterBB, LocalSize, EntryId_, VMap, Latches, LastHeader,
                    Idx);

  PreHeader_ = createUniformLoadBB(LastHeader);
  LastHeader->replacePhiUsesWith(&F.getEntryBlock(), PreHeader_);

  print();

  addRemappedDenseMapKeys(InstAllocaMap, VMap, RemappedInstAllocaMap);
  loadMultiSubCfgValues(InstAllocaMap, BaseInstAllocaMap, ContInstReplicaMap,
                        PreHeader_, VMap);
  loadUniformAndRecalcContValues(BaseInstAllocaMap, ContInstReplicaMap,
                                 PreHeader_, VMap);

  llvm::SmallVector<llvm::BasicBlock *, 8> BlocksToRemap{NewBlocks_.begin(),
                                                         NewBlocks_.end()};
  llvm::remapInstructionsInBlocks(BlocksToRemap, VMap);

  removeDeadPhiBlocks(BlocksToRemap);

  EntryBB_ = PreHeader_;
  ExitBB_ = Latches[0];
  ContIdx_ = Idx;
}

// remove incoming PHI blocks that no longer actually have an edge to the PHI
void SubCFG::removeDeadPhiBlocks(
    llvm::SmallVector<llvm::BasicBlock *, 8> &BlocksToRemap) const {
  for (auto *BB : BlocksToRemap) {
    llvm::SmallPtrSet<llvm::BasicBlock *, 4> Predecessors{llvm::pred_begin(BB),
                                                          llvm::pred_end(BB)};
    for (auto &I : *BB) {
      if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
        llvm::SmallVector<llvm::BasicBlock *, 4> IncomingBlocksToRemove;
        for (int IncomingIdx = 0; IncomingIdx < Phi->getNumIncomingValues();
             ++IncomingIdx) {
          auto *IncomingBB = Phi->getIncomingBlock(IncomingIdx);
          if (!Predecessors.contains(IncomingBB))
            IncomingBlocksToRemove.push_back(IncomingBB);
        }
        for (auto *IncomingBB : IncomingBlocksToRemove) {
          llvm::errs() << "[SubCFG] Remove incoming block "
                       << IncomingBB->getName() << " from PHI " << *Phi << "\n";
          Phi->removeIncomingValue(IncomingBB);
          llvm::errs() << "[SubCFG] Removed incoming block "
                       << IncomingBB->getName() << " from PHI " << *Phi << "\n";
        }
      }
    }
  }
}

// // check if a contiguous value can be tracked back to only uniform values and
// // the wi-loop indvar currently cannot track back the value through PHI
// nodes. bool dontArrayifyContiguousValues(
//     llvm::Instruction &I,
//     llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
//     &BaseInstAllocaMap, llvm::DenseMap<llvm::Instruction *,
//                    llvm::SmallVector<llvm::Instruction *, 8>>
//         &ContInstReplicaMap,
//     llvm::Instruction *AllocaIP, size_t ReqdArrayElements, llvm::Value
//     *IndVar, pocl::VariableUniformityAnalysis &VecInfo) {
//   // is cont indvar
//   if (VecInfo.isPinned(I))
//     return true;

//   llvm::SmallVector<llvm::Instruction *, 4> WL;
//   llvm::SmallPtrSet<llvm::Instruction *, 8> UniformValues;
//   llvm::SmallVector<llvm::Instruction *, 8> ContiguousInsts;
//   llvm::SmallPtrSet<llvm::Value *, 8> LookedAt;
//   llvm::errs() << "[SubCFG] IndVar: " << *IndVar << "\n";
//   WL.push_back(&I);
//   while (!WL.empty()) {
//     auto *WLValue = WL.pop_back_val();
//     if (auto *WLI = llvm::dyn_cast<llvm::Instruction>(WLValue))
//       for (auto *V : WLI->operand_values()) {
//         llvm::errs() << "[SubCFG] Considering: " << *V << "\n";

//         if (V == IndVar || VecInfo.isPinned(*V))
//           continue;
//         // todo: fix PHIs
//         if (LookedAt.contains(V))
//           return false;
//         LookedAt.insert(V);

//         // collect cont and uniform source values
//         if (auto *OpI = llvm::dyn_cast<llvm::Instruction>(V)) {
//           if (VecInfo.getVectorShape(*OpI).isContiguous()) {
//             WL.push_back(OpI);
//             ContiguousInsts.push_back(OpI);
//           } else if (!UniformValues.contains(OpI))
//             UniformValues.insert(OpI);
//         }
//       }
//   }
//   for (auto *UI : UniformValues) {
//     llvm::errs() << "[SubCFG] UniValue to store: " << *UI << "\n";
//     if (BaseInstAllocaMap.lookup(UI))
//       continue;
//     llvm::errs()
//         << "[SubCFG] Store required uniform value to single element alloca "
//         << I << "\n";
//     auto *Alloca = arrayifyInstruction(AllocaIP, UI, IndVar, 1);
//     BaseInstAllocaMap.insert({UI, Alloca});
//     VecInfo.setVectorShape(*Alloca, pocl::VectorShape::uni());
//   }
//   ContInstReplicaMap.insert({&I, ContiguousInsts});
//   return true;
// }

// creates array allocas for values that are identified as spanning multiple
// subcfgs
void SubCFG::arrayifyMultiSubCfgValues(
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &InstAllocaMap,
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &BaseInstAllocaMap,
    llvm::DenseMap<llvm::Instruction *,
                   llvm::SmallVector<llvm::Instruction *, 8>>
        &ContInstReplicaMap,
    llvm::ArrayRef<SubCFG> SubCFGs, llvm::Instruction *AllocaIP,
    size_t ReqdArrayElements, pocl::VariableUniformityAnalysis &VecInfo) {
  llvm::SmallPtrSet<llvm::BasicBlock *, 16> OtherCFGBlocks;
  for (auto &Cfg : SubCFGs) {
    if (&Cfg != this)
      OtherCFGBlocks.insert(Cfg.Blocks_.begin(), Cfg.Blocks_.end());
  }

  for (auto *BB : Blocks_) {
    for (auto &I : *BB) {
      if (&I == ContIdx_)
        continue;
      if (InstAllocaMap.lookup(&I))
        continue;
      // if any use is in another subcfg
      if (anyOfUsers<llvm::Instruction>(
              &I, [&OtherCFGBlocks, this, &I](auto *UI) {
                return UI->getParent() != I.getParent() &&
                       OtherCFGBlocks.contains(UI->getParent());
              })) {
        // load from an alloca, just widen alloca
        if (auto *LInst = llvm::dyn_cast<llvm::LoadInst>(&I))
          if (auto *Alloca = getLoopStateAllocaForLoad(*LInst)) {
            InstAllocaMap.insert({&I, Alloca});
            continue;
          }
        // GEP from already widened alloca: reuse alloca
        if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I))
          if (GEP->hasMetadata(pocl::MDKind::Arrayified)) {
            InstAllocaMap.insert(
                {&I, llvm::cast<llvm::AllocaInst>(GEP->getPointerOperand())});
            continue;
          }

#ifndef HIPSYCL_NO_PHIS_IN_SPLIT
        // if value is uniform, just store to 1-wide alloca
        if (VecInfo.isUniform(I.getFunction(), &I)) {
          llvm::errs()
              << "[SubCFG] Value uniform, store to single element alloca " << I
              << "\n";
          auto *Alloca = arrayifyInstruction(AllocaIP, &I, ContIdx_, 1);
          InstAllocaMap.insert({&I, Alloca});
          VecInfo.setUniform(I.getFunction(), Alloca);
          continue;
        }
#endif
        // #ifndef HIPSYCL_NO_CONTIGUOUS_VALUES
        //         // if contiguous, and can be recalculated, don't arrayify but
        //         store
        //         // uniform values and insts required for recalculation
        //         if (Shape.isContiguous()) {
        //           if (dontArrayifyContiguousValues(
        //                   I, BaseInstAllocaMap, ContInstReplicaMap, AllocaIP,
        //                   ReqdArrayElements, ContIdx_, VecInfo)) {
        //             llvm::errs() << "[SubCFG] Not arrayifying " << I << "\n";
        //             continue;
        //           }
        //         }
        // #endif
        // create wide alloca and store the value
        auto *Alloca =
            arrayifyInstruction(AllocaIP, &I, ContIdx_, ReqdArrayElements);
        InstAllocaMap.insert({&I, Alloca});
      }
    }
  }
}

void remapInstruction(llvm::Instruction *I, llvm::ValueToValueMapTy &VMap) {
  llvm::SmallVector<llvm::Value *, 8> WL{I->value_op_begin(),
                                         I->value_op_end()};
  for (auto *V : WL) {
    if (VMap.count(V))
      I->replaceUsesOfWith(V, VMap[V]);
  }
  llvm::errs() << "[SubCFG] remapped Inst " << *I << "\n";
}

// inserts loads from the loop state allocas for varying values that were
// identified as multi-subcfg values
void SubCFG::loadMultiSubCfgValues(
    const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
        &InstAllocaMap,
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &BaseInstAllocaMap,
    llvm::DenseMap<llvm::Instruction *,
                   llvm::SmallVector<llvm::Instruction *, 8>>
        &ContInstReplicaMap,
    llvm::BasicBlock *UniformLoadBB, llvm::ValueToValueMapTy &VMap) {
  llvm::Value *NewContIdx = VMap[ContIdx_];
  auto *LoadTerm = LoadBB_->getTerminator();
  auto *UniformLoadTerm = UniformLoadBB->getTerminator();
  llvm::IRBuilder Builder{LoadTerm};

  for (auto &InstAllocaPair : InstAllocaMap) {
    // If def not in sub CFG but a use of it is in the sub CFG
    if (std::find(Blocks_.begin(), Blocks_.end(),
                  InstAllocaPair.first->getParent()) == Blocks_.end()) {
      if (anyOfUsers<llvm::Instruction>(
              InstAllocaPair.first, [this](llvm::Instruction *UI) {
                return std::find(NewBlocks_.begin(), NewBlocks_.end(),
                                 UI->getParent()) != NewBlocks_.end();
              })) {
        if (auto *GEP =
                llvm::dyn_cast<llvm::GetElementPtrInst>(InstAllocaPair.first))
          if (auto *MDArrayified = GEP->getMetadata(pocl::MDKind::Arrayified)) {
            auto *NewGEP =
                llvm::cast<llvm::GetElementPtrInst>(Builder.CreateInBoundsGEP(
                    GEP->getType(), GEP->getPointerOperand(), NewContIdx,
                    GEP->getName() + "c"));
            NewGEP->setMetadata(pocl::MDKind::Arrayified, MDArrayified);
            VMap[InstAllocaPair.first] = NewGEP;
            continue;
          }
        auto *IP = LoadTerm;
        if (!InstAllocaPair.second->isArrayAllocation())
          IP = UniformLoadTerm;
        llvm::errs() << "[SubCFG] Load from Alloca " << *InstAllocaPair.second
                     << " in " << IP->getParent()->getName() << "\n";
        auto *Load = loadFromAlloca(InstAllocaPair.second, NewContIdx, IP,
                                    InstAllocaPair.first->getName());
        // copyDgbValues(InstAllocaPair.first, Load, IP);
        VMap[InstAllocaPair.first] = Load;
      }
    }
  }
}

// Inserts loads for the multi-subcfg values that were identified as uniform
// inside the wi-loop preheader. Additionally clones the instructions that were
// identified as contiguous \a ContInstReplicaMap inside the LoadBB_ to restore
// the contiguous value just from the uniform values and the wi-idx.
void SubCFG::loadUniformAndRecalcContValues(
    llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> &BaseInstAllocaMap,
    llvm::DenseMap<llvm::Instruction *,
                   llvm::SmallVector<llvm::Instruction *, 8>>
        &ContInstReplicaMap,
    llvm::BasicBlock *UniformLoadBB, llvm::ValueToValueMapTy &VMap) {
  llvm::ValueToValueMapTy UniVMap;
  auto *LoadTerm = LoadBB_->getTerminator();
  auto *UniformLoadTerm = UniformLoadBB->getTerminator();
  llvm::Value *NewContIdx = VMap[this->ContIdx_];
  UniVMap[this->ContIdx_] = NewContIdx;

  // copy local id load value to univmap
  for (size_t D = 0; D < this->Dim; ++D) {
    auto *Load = getLoadForGlobalVariable(*this->LoadBB_->getParent(),
                                          LocalIdGlobalNames[D]);
    UniVMap[Load] = VMap[Load];
  }

  // load uniform values from allocas
  for (auto &InstAllocaPair : BaseInstAllocaMap) {
    auto *IP = UniformLoadTerm;
    llvm::errs() << "[SubCFG] Load base value from Alloca "
                 << *InstAllocaPair.second << " in "
                 << IP->getParent()->getName() << "\n";
    auto *Load = loadFromAlloca(InstAllocaPair.second, NewContIdx, IP,
                                InstAllocaPair.first->getName());
    // copyDgbValues(InstAllocaPair.first, Load, IP);
    UniVMap[InstAllocaPair.first] = Load;
  }

  // get a set of unique contiguous instructions
  llvm::SmallPtrSet<llvm::Instruction *, 16> UniquifyInsts;
  for (auto &Pair : ContInstReplicaMap) {
    UniquifyInsts.insert(Pair.first);
    for (auto &Target : Pair.second)
      UniquifyInsts.insert(Target);
  }

  auto OrderedInsts = topoSortInstructions(UniquifyInsts);

  llvm::SmallPtrSet<llvm::Instruction *, 16> InstsToRemap;
  // clone the contiguous instructions to restore the used values
  for (auto *I : OrderedInsts) {
    if (UniVMap.count(I))
      continue;

    llvm::errs() << "[SubCFG] Clone cont instruction and operands of: " << *I
                 << " to " << LoadTerm->getParent()->getName() << "\n";
    auto *IClone = I->clone();
    IClone->insertBefore(LoadTerm);
    InstsToRemap.insert(IClone);
    UniVMap[I] = IClone;
    if (VMap.count(I) == 0)
      VMap[I] = IClone;
    llvm::errs() << "[SubCFG] Clone cont instruction: " << *IClone << "\n";
  }

  // finally remap the singular instructions to use the other cloned contiguous
  // instructions / uniform values
  for (auto *IToRemap : InstsToRemap)
    remapInstruction(IToRemap, UniVMap);
}
llvm::SmallVector<llvm::Instruction *, 16> SubCFG::topoSortInstructions(
    const llvm::SmallPtrSet<llvm::Instruction *, 16> &UniquifyInsts) const {
  llvm::SmallVector<llvm::Instruction *, 16> OrderedInsts(UniquifyInsts.size());
  std::copy(UniquifyInsts.begin(), UniquifyInsts.end(), OrderedInsts.begin());

  auto IsUsedBy = [](llvm::Instruction *LHS, llvm::Instruction *RHS) {
    for (auto *U : LHS->users()) {
      if (U == RHS)
        return true;
    }
    return false;
  };
  for (int I = 0; I < OrderedInsts.size(); ++I) {
    int InsertAt = I;
    for (int J = OrderedInsts.size() - 1; J > I; --J) {
      if (IsUsedBy(OrderedInsts[J], OrderedInsts[I])) {
        InsertAt = J;
        break;
      }
    }
    if (InsertAt != I) {
      auto *Tmp = OrderedInsts[I];
      for (int J = I + 1; J <= InsertAt; ++J) {
        OrderedInsts[J - 1] = OrderedInsts[J];
      }
      OrderedInsts[InsertAt] = Tmp;
      --I;
    }
  }
  return OrderedInsts;
}

llvm::BasicBlock *
SubCFG::createUniformLoadBB(llvm::BasicBlock *OuterMostHeader) {
  auto *LoadBB = llvm::BasicBlock::Create(
      OuterMostHeader->getContext(),
      "uniloadblock.subcfg." + llvm::Twine{EntryId_} + "b",
      OuterMostHeader->getParent(), OuterMostHeader);
  llvm::IRBuilder Builder{LoadBB, LoadBB->getFirstInsertionPt()};
  Builder.CreateBr(OuterMostHeader);
  return LoadBB;
}

llvm::BasicBlock *SubCFG::createLoadBB(llvm::ValueToValueMapTy &VMap) {
  auto *NewEntry =
      llvm::cast<llvm::BasicBlock>(static_cast<llvm::Value *>(VMap[EntryBB_]));
  auto *LoadBB = llvm::BasicBlock::Create(
      NewEntry->getContext(), "loadblock.subcfg." + llvm::Twine{EntryId_} + "b",
      NewEntry->getParent(), NewEntry);
  llvm::IRBuilder Builder{LoadBB, LoadBB->getFirstInsertionPt()};
  Builder.CreateBr(NewEntry);
  return LoadBB;
}

// if the kernel contained a loop, it is possible, that values inside a single
// subcfg don't dominate their uses inside the same subcfg. This function
// identifies and fixes those values.
void SubCFG::fixSingleSubCfgValues(
    llvm::DominatorTree &DT,
    const llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *>
        &RemappedInstAllocaMap,
    std::size_t ReqdArrayElements, pocl::VariableUniformityAnalysis &VecInfo) {

  auto *AllocaIP =
      LoadBB_->getParent()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
  auto *LoadIP = LoadBB_->getTerminator();
  auto *UniLoadIP = PreHeader_->getTerminator();
  llvm::IRBuilder Builder{LoadIP};

  llvm::DenseMap<llvm::Instruction *, llvm::Instruction *> InstLoadMap;

  for (auto *BB : NewBlocks_) {
    llvm::SmallVector<llvm::Instruction *, 16> Insts{};
    std::transform(BB->begin(), BB->end(), std::back_inserter(Insts),
                   [](auto &I) { return &I; });
    for (auto *Inst : Insts) {
      auto &I = *Inst;
      for (auto *OPV : I.operand_values()) {
        // check if all operands dominate the instruction -> otherwise we have
        // to fix it
        if (auto *OPI = llvm::dyn_cast<llvm::Instruction>(OPV);
            OPI && !DT.dominates(OPI, &I)) {
          if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
            // if a PHI node, we have to check that the incoming values dominate
            // the terminators of the incoming block..
            bool FoundIncoming = false;
            for (auto &Incoming : Phi->incoming_values()) {
              if (OPV == Incoming.get()) {
                auto *IncomingBB = Phi->getIncomingBlock(Incoming);
                if (DT.dominates(OPI, IncomingBB->getTerminator())) {
                  FoundIncoming = true;
                  break;
                }
              }
            }
            if (FoundIncoming)
              continue;
          }
          llvm::errs() << "Instruction not dominated " << I
                       << " operand: " << *OPI << "\n";

          if (auto *Load = InstLoadMap.lookup(OPI))
            // if the already inserted Load does not dominate I, we must create
            // another load.
            if (DT.dominates(Load, &I)) {
              I.replaceUsesOfWith(OPI, Load);
              continue;
            }

          if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(OPI))
            if (auto *MDArrayified =
                    GEP->getMetadata(pocl::MDKind::Arrayified)) {
              auto *NewGEP =
                  llvm::cast<llvm::GetElementPtrInst>(Builder.CreateInBoundsGEP(
                      GEP->getType(), GEP->getPointerOperand(), ContIdx_,
                      GEP->getName() + "c"));
              NewGEP->setMetadata(pocl::MDKind::Arrayified, MDArrayified);
              I.replaceUsesOfWith(OPI, NewGEP);
              InstLoadMap.insert({OPI, NewGEP});
              continue;
            }

          llvm::AllocaInst *Alloca = nullptr;
          if (auto *RemAlloca = RemappedInstAllocaMap.lookup(OPI))
            Alloca = RemAlloca;
          if (auto *LInst = llvm::dyn_cast<llvm::LoadInst>(OPI))
            Alloca = getLoopStateAllocaForLoad(*LInst);
          if (!Alloca) {
            llvm::errs() << "[SubCFG] No alloca, yet for " << *OPI << "\n";
            //            if (VecInfo.getVectorShape(I).isUniform())
            //              Alloca = arrayifyInstruction(AllocaIP, OPI,
            //              ContIdx_, 1);
            //            else
            Alloca =
                arrayifyInstruction(AllocaIP, OPI, ContIdx_, ReqdArrayElements);
            // VecInfo.setVectorShape(*Alloca, VecInfo.getVectorShape(I));
          }

#ifdef HIPSYCL_NO_PHIS_IN_SPLIT
          // in split loop, OPI might be used multiple times, get the user,
          // dominating this user and insert load there
          llvm::Instruction *NewIP = &I;
          for (auto *U : OPI->users()) {
            if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
                UI && DT.dominates(UI, NewIP)) {
              NewIP = UI;
            }
          }
#else
          // doesn't happen if we keep the PHIs
          auto *NewIP = LoadIP;
          if (!Alloca->isArrayAllocation())
            NewIP = UniLoadIP;
#endif

          auto *Load = loadFromAlloca(Alloca, ContIdx_, NewIP, OPI->getName());
          // copyDgbValues(OPI, Load, NewIP);

#ifdef HIPSYCL_NO_PHIS_IN_SPLIT
          I.replaceUsesOfWith(OPI, Load);
          InstLoadMap.insert({OPI, Load});
#else
          // if a loop is conditionally split, the first block in a subcfg might
          // have another incoming edge, need to insert a PHI node then
          const auto NumPreds =
              std::distance(llvm::pred_begin(BB), llvm::pred_end(BB));
          if (!llvm::isa<llvm::PHINode>(I) && NumPreds > 1 &&
              std::find(llvm::pred_begin(BB), llvm::pred_end(BB), LoadBB_) !=
                  llvm::pred_end(BB)) {
            Builder.SetInsertPoint(BB, BB->getFirstInsertionPt());
            auto *PHINode =
                Builder.CreatePHI(Load->getType(), NumPreds, I.getName());
            for (auto *PredBB : llvm::predecessors(BB))
              if (PredBB == LoadBB_)
                PHINode->addIncoming(Load, PredBB);
              else
                PHINode->addIncoming(OPV, PredBB);

            I.replaceUsesOfWith(OPI, PHINode);
            InstLoadMap.insert({OPI, PHINode});
          } else {
            I.replaceUsesOfWith(OPI, Load);
            InstLoadMap.insert({OPI, Load});
          }
#endif
        }
      }
    }
  }
}

llvm::BasicBlock *createUnreachableBlock(llvm::Function &F) {
  auto *Default =
      llvm::BasicBlock::Create(F.getContext(), "cbs.while.default", &F);
  llvm::IRBuilder Builder{Default, Default->getFirstInsertionPt()};
  Builder.CreateUnreachable();
  return Default;
}

// create the actual while loop around the subcfgs and the switch instruction to
// select the next subCFG based on the value in \a LastBarrierIdStorage
llvm::BasicBlock *
generateWhileSwitchAround(llvm::BasicBlock *PreHeader,
                          llvm::BasicBlock *OldEntry, llvm::BasicBlock *Exit,
                          llvm::AllocaInst *LastBarrierIdStorage,
                          std::vector<SubCFG> &SubCFGs) {
  auto &F = *PreHeader->getParent();
  auto &M = *F.getParent();
  const auto &DL = M.getDataLayout();

  auto *WhileHeader =
      llvm::BasicBlock::Create(PreHeader->getContext(), "cbs.while.header",
                               PreHeader->getParent(), OldEntry);
  llvm::IRBuilder Builder{WhileHeader, WhileHeader->getFirstInsertionPt()};
  auto *LastID =
      Builder.CreateLoad(LastBarrierIdStorage->getAllocatedType(),
                         LastBarrierIdStorage, "cbs.while.last_barr.load");
  auto *Switch =
      Builder.CreateSwitch(LastID, createUnreachableBlock(F), SubCFGs.size());
  for (auto &Cfg : SubCFGs) {
    Switch->addCase(Builder.getIntN(DL.getLargestLegalIntTypeSizeInBits(),
                                    Cfg.getEntryId()),
                    Cfg.getEntry());
    Cfg.getEntry()->replacePhiUsesWith(PreHeader, WhileHeader);
    Cfg.getExit()->getTerminator()->replaceSuccessorWith(Exit, WhileHeader);
  }
  Switch->addCase(
      Builder.getIntN(DL.getLargestLegalIntTypeSizeInBits(), ExitBarrierId),
      Exit);

  Builder.SetInsertPoint(PreHeader->getTerminator());
  Builder.CreateStore(
      llvm::ConstantInt::get(LastBarrierIdStorage->getAllocatedType(),
                             EntryBarrierId),
      LastBarrierIdStorage);
  PreHeader->getTerminator()->replaceSuccessorWith(OldEntry, WhileHeader);
  return WhileHeader;
}

// drops all lifetime intrinsics - they are misinforming ASAN otherwise (and are
// not really fixable at the right scope..)
void purgeLifetime(SubCFG &Cfg) {
  llvm::SmallVector<llvm::Instruction *, 8> ToDelete;
  for (auto *BB : Cfg.getNewBlocks())
    for (auto &I : *BB)
      if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I))
        if (CI->getCalledFunction())
          if (CI->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::lifetime_start ||
              CI->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::lifetime_end)
            ToDelete.push_back(CI);

  for (auto *I : ToDelete)
    I->eraseFromParent();
}

// fills \a Hull with all transitive users of \a Alloca
void fillUserHull(llvm::AllocaInst *Alloca,
                  llvm::SmallVectorImpl<llvm::Instruction *> &Hull) {
  llvm::SmallVector<llvm::Instruction *, 8> WL;
  std::transform(Alloca->user_begin(), Alloca->user_end(),
                 std::back_inserter(WL),
                 [](auto *U) { return llvm::cast<llvm::Instruction>(U); });
  llvm::SmallPtrSet<llvm::Instruction *, 32> AlreadySeen;
  while (!WL.empty()) {
    auto *I = WL.pop_back_val();
    AlreadySeen.insert(I);
    Hull.push_back(I);
    for (auto *U : I->users()) {
      if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
        if (!AlreadySeen.contains(UI))
          if (UI->mayReadOrWriteMemory() || UI->getType()->isPointerTy())
            WL.push_back(UI);
      }
    }
  }
}

template <class PtrSet> struct PtrSetWrapper {
  explicit PtrSetWrapper(PtrSet &PtrSetArg) : Set(PtrSetArg) {}
  PtrSet &Set;
  using iterator = typename PtrSet::iterator;
  using value_type = typename PtrSet::value_type;
  template <class IT, class ValueT> IT insert(IT, const ValueT &Value) {
    return Set.insert(Value).first;
  }
};

// checks if all uses of an alloca are in just a single subcfg (doesn't have to
// be arrayified!)
bool isAllocaSubCfgInternal(llvm::AllocaInst *Alloca,
                            const std::vector<SubCFG> &SubCfgs,
                            const llvm::DominatorTree &DT) {
  llvm::SmallPtrSet<llvm::BasicBlock *, 16> UserBlocks;
  {
    llvm::SmallVector<llvm::Instruction *, 32> Users;
    fillUserHull(Alloca, Users);
    PtrSetWrapper<decltype(UserBlocks)> Wrapper{UserBlocks};
    std::transform(Users.begin(), Users.end(),
                   std::inserter(Wrapper, UserBlocks.end()),
                   [](auto *I) { return I->getParent(); });
  }

  for (auto &SubCfg : SubCfgs) {
    llvm::SmallPtrSet<llvm::BasicBlock *, 8> SubCfgSet{
        SubCfg.getNewBlocks().begin(), SubCfg.getNewBlocks().end()};
    if (std::any_of(
            UserBlocks.begin(), UserBlocks.end(),
            [&SubCfgSet](auto *BB) { return SubCfgSet.contains(BB); }) &&
        !std::all_of(UserBlocks.begin(), UserBlocks.end(),
                     [&SubCfgSet, Alloca](auto *BB) {
                       if (SubCfgSet.contains(BB)) {
                         return true;
                       }
#ifdef DEBUG_SUBCFG_FORMATION
                       llvm::errs()
                           << "[SubCFG] BB not in subcfgset: " << BB->getName()
                           << " for alloca: ";
                       Alloca->print(llvm::outs());
                       llvm::outs() << "\n";
#endif
                       return false;
                     }))
      return false;
  }

  return true;
}

void arrayifyAllocas(llvm::BasicBlock *EntryBlock, llvm::DominatorTree &DT,
                     std::vector<SubCFG> &SubCfgs,
                     std::size_t ReqdArrayElements,
                     pocl::VariableUniformityAnalysis &VecInfo) {
  auto *MDAlloca = llvm::MDNode::get(
      EntryBlock->getContext(),
      {llvm::MDString::get(EntryBlock->getContext(), "poclLoopState")});

  llvm::SmallPtrSet<llvm::BasicBlock *, 32> SubCfgsBlocks;
  for (auto &SubCfg : SubCfgs)
    SubCfgsBlocks.insert(SubCfg.getNewBlocks().begin(),
                         SubCfg.getNewBlocks().end());

  llvm::SmallVector<llvm::AllocaInst *, 8> WL;
  for (auto &I : *EntryBlock) {
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
      if (Alloca->hasMetadata(pocl::MDKind::Arrayified))
        continue; // already arrayified
      if (anyOfUsers<llvm::Instruction>(
              Alloca, [&SubCfgsBlocks](llvm::Instruction *UI) {
                return !SubCfgsBlocks.contains(UI->getParent());
              }))
        continue;
      if (!isAllocaSubCfgInternal(Alloca, SubCfgs, DT))
        WL.push_back(Alloca);
    }
  }

  for (auto *I : WL) {
    // todo: can we somehow enable this..?
    //    if (VecInfo.getVectorShape(*I).isUniform()) {
    //      llvm::errs() << "[SubCFG] Not arrayifying alloca " << *I << "\n";
    //      continue;
    //    }
    llvm::IRBuilder AllocaBuilder{I};
    llvm::Type *T = I->getAllocatedType();
    if (auto *ArrSizeC = llvm::dyn_cast<llvm::ConstantInt>(I->getArraySize())) {
      auto ArrSize = ArrSizeC->getLimitedValue();
      if (ArrSize > 1) {
        T = llvm::ArrayType::get(T, ArrSize);
        llvm::errs() << "Caution, alloca was array\n";
      }
    }

    auto *Alloca = AllocaBuilder.CreateAlloca(
        T, AllocaBuilder.getInt32(ReqdArrayElements), I->getName() + "_alloca");
    Alloca->setAlignment(llvm::Align{pocl::DefaultAlignment});
    Alloca->setMetadata(pocl::MDKind::Arrayified, MDAlloca);

    for (auto &SubCfg : SubCfgs) {
      auto *GepIp = SubCfg.getLoadBB()->getFirstNonPHIOrDbgOrLifetime();

      llvm::IRBuilder LoadBuilder{GepIp};
      auto *GEP =
          llvm::cast<llvm::GetElementPtrInst>(LoadBuilder.CreateInBoundsGEP(
              Alloca->getAllocatedType(), Alloca, SubCfg.getContiguousIdx(),
              I->getName() + "_gep"));
      GEP->setMetadata(pocl::MDKind::Arrayified, MDAlloca);

      llvm::replaceDominatedUsesWith(I, GEP, DT, SubCfg.getLoadBB());
    }
    I->eraseFromParent();
  }
}

void moveAllocasToEntry(llvm::Function &F,
                        llvm::ArrayRef<llvm::BasicBlock *> Blocks) {
  llvm::SmallVector<llvm::AllocaInst *, 4> AllocaWL;
  for (auto *BB : Blocks)
    for (auto &I : *BB)
      if (auto *AllocaInst = llvm::dyn_cast<llvm::AllocaInst>(&I))
        AllocaWL.push_back(AllocaInst);
  for (auto *I : AllocaWL)
    if (F.getEntryBlock().size() == 1)
      I->moveBefore(F.getEntryBlock().getFirstNonPHI());
    else
      I->moveAfter(F.getEntryBlock().getFirstNonPHI());
}

llvm::DenseMap<llvm::BasicBlock *, size_t>
getBarrierIds(llvm::BasicBlock *Entry,
              llvm::SmallPtrSetImpl<llvm::BasicBlock *> &ExitingBlocks,
              llvm::ArrayRef<llvm::BasicBlock *> Blocks) {
  llvm::DenseMap<llvm::BasicBlock *, size_t> Barriers;
  // mark exit barrier with the corresponding id:
  for (auto *BB : ExitingBlocks)
    Barriers[BB] = ExitBarrierId;
  // mark entry barrier with the corresponding id:
  Barriers[Entry] = EntryBarrierId;

  // store all other barrier blocks with a unique id:
  size_t BarrierId = 1;
  for (auto *BB : Blocks)
    if (Barriers.find(BB) == Barriers.end() && Barrier::hasOnlyBarrier(BB))
      Barriers.insert({BB, BarrierId++});
  return Barriers;
}
void formSubCfgs(llvm::Function &F, llvm::LoopInfo &LI, llvm::DominatorTree &DT,
                 llvm::PostDominatorTree &PDT,
                 pocl::VariableUniformityAnalysis &VUA) {
#ifdef DEBUG_SUBCFG_FORMATION
  F.viewCFG();
#endif

  // const std::size_t Dim = getRangeDim(F);
  // llvm::errs() << "[SubCFG] Kernel is " << Dim << "-dimensional\n";

  std::array<size_t, 3> LocalSizes;
  getModuleIntMetadata(*F.getParent(), "WGLocalSizeX", LocalSizes[0]);
  getModuleIntMetadata(*F.getParent(), "WGLocalSizeY", LocalSizes[1]);
  getModuleIntMetadata(*F.getParent(), "WGLocalSizeZ", LocalSizes[2]);
  bool WGDynamicLocalSize{};
  getModuleBoolMetadata(*F.getParent(), "WGDynamicLocalSize",
                        WGDynamicLocalSize);

  std::size_t Dim = 3;
  if (LocalSizes[2] == 1 && !WGDynamicLocalSize) {
    if (LocalSizes[1] == 1)
      Dim = 1;
    else
      Dim = 2;
  }

  const auto LocalSize =
      getLocalSizeValues(F, LocalSizes, WGDynamicLocalSize, Dim);

  const size_t ReqdArrayElements = pocl::NumArrayElements; // todo: specialize

  auto *Entry = &F.getEntryBlock();

  std::vector<llvm::BasicBlock *> Blocks;
  Blocks.reserve(std::distance(F.begin(), F.end()));
  std::transform(F.begin(), F.end(), std::back_inserter(Blocks),
                 [](auto &BB) { return &BB; });

  // non-entry block Allocas are considered broken, move to entry.
  moveAllocasToEntry(F, Blocks);

  // auto RImpl = getRegion(F, LI, Blocks);
  // pocl::Region R{*RImpl};
  // auto VecInfo = getVectorizationInfo(F, R, LI, DT, PDT, Dim);

  llvm::SmallPtrSet<llvm::BasicBlock *, 2> ExitingBlocks;
  for (auto *BB : Blocks) {
    if (BB->getTerminator()->getNumSuccessors() == 0)
      ExitingBlocks.insert(BB);
  }

  if (ExitingBlocks.empty()) {
    llvm::errs() << "[SubCFG] Invalid kernel! No kernel exits!\n";
    llvm_unreachable("[SubCFG] Invalid kernel! No kernel exits!\n");
  }

  auto Barriers = getBarrierIds(Entry, ExitingBlocks, Blocks);

  const llvm::DataLayout &DL = F.getParent()->getDataLayout();
  llvm::IRBuilder Builder{F.getEntryBlock().getFirstNonPHI()};
  auto *LastBarrierIdStorage = Builder.CreateAlloca(
      DL.getLargestLegalIntType(F.getContext()), nullptr, "LastBarrierId");

  // get a common (pseudo) index value to be replaced by the actual index later
  Builder.SetInsertPoint(F.getEntryBlock().getTerminator());
  auto *IndVarT =
      getLoadForGlobalVariable(F, LocalIdGlobalNames[Dim - 1])->getType();
  llvm::Instruction *IndVar = Builder.CreateLoad(
      IndVarT, llvm::UndefValue::get(llvm::PointerType::get(IndVarT, 0)));
  // VecInfo.setPinnedShape(*IndVar, pocl::VectorShape::cont()); // todo:
  // reeval, but should already be set..

  // create subcfgs
  std::vector<SubCFG> SubCFGs;
  for (auto &BIt : Barriers) {
    llvm::errs() << "Create SubCFG from " << BIt.first->getName() << "("
                 << BIt.first << ") id: " << BIt.second << "\n";
    if (BIt.second != ExitBarrierId)
      SubCFGs.emplace_back(BIt.first, LastBarrierIdStorage, Barriers, IndVar,
                           Dim);
  }

  llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> InstAllocaMap;
  llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> BaseInstAllocaMap;
  llvm::DenseMap<llvm::Instruction *, llvm::SmallVector<llvm::Instruction *, 8>>
      InstContReplicaMap;

  for (auto &Cfg : SubCFGs)
    Cfg.arrayifyMultiSubCfgValues(
        InstAllocaMap, BaseInstAllocaMap, InstContReplicaMap, SubCFGs,
        F.getEntryBlock().getFirstNonPHI(), ReqdArrayElements, VUA);

  llvm::DenseMap<llvm::Instruction *, llvm::AllocaInst *> RemappedInstAllocaMap;
  for (auto &Cfg : SubCFGs) {
    Cfg.print();
    Cfg.replicate(F, InstAllocaMap, BaseInstAllocaMap, InstContReplicaMap,
                  RemappedInstAllocaMap, *ExitingBlocks.begin(), LocalSize);
    purgeLifetime(Cfg);
  }

  llvm::BasicBlock *WhileHeader = nullptr;
  WhileHeader = generateWhileSwitchAround(
      &F.getEntryBlock(), F.getEntryBlock().getSingleSuccessor(),
      *ExitingBlocks.begin(), LastBarrierIdStorage, SubCFGs);

  llvm::removeUnreachableBlocks(F);

  DT.recalculate(F);
  arrayifyAllocas(&F.getEntryBlock(), DT, SubCFGs, ReqdArrayElements, VUA);

  for (auto &Cfg : SubCFGs) {
    Cfg.fixSingleSubCfgValues(DT, RemappedInstAllocaMap, ReqdArrayElements,
                              VUA);
  }

  IndVar->eraseFromParent();

#ifdef DEBUG_SUBCFG_FORMATION
  F.viewCFG();
#endif
  assert(!llvm::verifyFunction(F, &llvm::errs()) &&
         "Function verification failed");

  // simplify while loop to get single latch that isn't marked as wi-loop to
  // prevent misunderstandings.
  auto *WhileLoop = updateDtAndLi(LI, DT, WhileHeader, F);
  llvm::simplifyLoop(WhileLoop, &DT, &LI, nullptr, nullptr, nullptr, false);
}

void createLoopsAroundKernel(llvm::Function &F, llvm::DominatorTree &DT,
                             llvm::LoopInfo &LI, llvm::PostDominatorTree &PDT) {
#if LLVM_VERSION_MAJOR >= 13
#define HIPSYCL_LLVM_BEFORE , true
#else
#define HIPSYCL_LLVM_BEFORE
#endif

  auto *Body = llvm::SplitBlock(&F.getEntryBlock(),
                                &*F.getEntryBlock().getFirstInsertionPt(), &DT,
                                &LI, nullptr, "wibody" HIPSYCL_LLVM_BEFORE);
#ifdef DEBUG_SUBCFG_FORMATION
  F.viewCFG();
#endif

#if LLVM_VERSION_MAJOR >= 13
  Body = Body->getSingleSuccessor();
#endif

  llvm::BasicBlock *ExitBB = nullptr;
  for (auto &BB : F) {
    if (BB.getTerminator()->getNumSuccessors() == 0) {
      ExitBB = llvm::SplitBlock(&BB, BB.getTerminator(), &DT, &LI, nullptr,
                                "exit" HIPSYCL_LLVM_BEFORE);
#if LLVM_VERSION_MAJOR >= 13
      if (Body == &BB)
        std::swap(Body, ExitBB);
      ExitBB = &BB;
#endif
      break;
    }
  }
#undef HIPSYCL_LLVM_BEFORE

  llvm::SmallVector<llvm::BasicBlock *, 8> Blocks{};
  Blocks.reserve(std::distance(F.begin(), F.end()));
  std::transform(F.begin(), F.end(), std::back_inserter(Blocks),
                 [](auto &BB) { return &BB; });

  moveAllocasToEntry(F, Blocks);

  std::array<size_t, 3> LocalSizes;
  getModuleIntMetadata(*F.getParent(), "WGLocalSizeX", LocalSizes[0]);
  getModuleIntMetadata(*F.getParent(), "WGLocalSizeY", LocalSizes[1]);
  getModuleIntMetadata(*F.getParent(), "WGLocalSizeZ", LocalSizes[2]);
  bool WGDynamicLocalSize{};
  getModuleBoolMetadata(*F.getParent(), "WGDynamicLocalSize",
                        WGDynamicLocalSize);

  std::size_t Dim = 3;
  if (LocalSizes[2] == 1 && !WGDynamicLocalSize) {
    if (LocalSizes[1] == 1)
      Dim = 1;
    else
      Dim = 2;
  }

  const auto LocalSize =
      getLocalSizeValues(F, LocalSizes, WGDynamicLocalSize, Dim);

  // insert dummy induction variable that can be easily identified and replaced
  // later
  llvm::IRBuilder Builder{F.getEntryBlock().getTerminator()};
  auto *IndVarT =
      getLoadForGlobalVariable(F, LocalIdGlobalNames[Dim - 1])->getType();
  llvm::Value *Idx = Builder.CreateLoad(
      IndVarT, llvm::UndefValue::get(llvm::PointerType::get(IndVarT, 0)));

  llvm::ValueToValueMapTy VMap;
  llvm::SmallVector<llvm::BasicBlock *, 3> Latches;
  auto *LastHeader = Body;

  createLoopsAround(F, ExitBB, LocalSize, 0, VMap, Latches, LastHeader, Idx);

  F.getEntryBlock().getTerminator()->setSuccessor(0, LastHeader);
  llvm::remapInstructionsInBlocks(Blocks, VMap);

  // remove uses of the undefined global id variables
  for (int D = 0; D < Dim; ++D)
    if (auto *Load = llvm::cast_or_null<llvm::LoadInst>(
            getLoadForGlobalVariable(F, LocalIdGlobalNames[D])))
      Load->eraseFromParent();
#ifdef DEBUG_SUBCFG_FORMATION
  F.viewCFG();
#endif
}

static llvm::RegisterPass<pocl::SubCfgFormationPassLegacy>
    X("subcfgformation", "Form SubCFGs according to CBS");
} // namespace

namespace pocl {
void SubCfgFormationPassLegacy::getAnalysisUsage(
    llvm::AnalysisUsage &AU) const {
  AU.addRequired<llvm::LoopInfoWrapperPass>();
  AU.addRequiredTransitive<llvm::DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<llvm::PostDominatorTreeWrapperPass>();
  AU.addRequired<VariableUniformityAnalysis>();
  AU.addPreserved<VariableUniformityAnalysis>();
  AU.addRequired<WorkitemHandlerChooser>();
  AU.addPreserved<WorkitemHandlerChooser>();
}

bool SubCfgFormationPassLegacy::runOnFunction(llvm::Function &F) {
  if (!Workgroup::isKernelToProcess(F))
    return false;

  if (getAnalysis<pocl::WorkitemHandlerChooser>().chosenHandler() !=
      pocl::WorkitemHandlerChooser::POCL_WIH_CBS)
    return false;

  llvm::errs() << "[SubCFG] Form SubCFGs in " << F.getName() << "\n";

  auto &DT = getAnalysis<llvm::DominatorTreeWrapperPass>().getDomTree();
  auto &PDT =
      getAnalysis<llvm::PostDominatorTreeWrapperPass>().getPostDomTree();
  auto &LI = getAnalysis<llvm::LoopInfoWrapperPass>().getLoopInfo();
  auto &VUA = getAnalysis<pocl::VariableUniformityAnalysis>();

  if (Workgroup::hasWorkgroupBarriers(F))
    formSubCfgs(F, LI, DT, PDT, VUA);
  else
    createLoopsAroundKernel(F, DT, LI, PDT);

  return false;
}

char SubCfgFormationPassLegacy::ID = 0;

// llvm::PreservedAnalyses
// SubCfgFormationPass::run(llvm::Function &F, llvm::FunctionAnalysisManager
// &AM) {
//   auto &MAM = AM.getResult<llvm::ModuleAnalysisManagerFunctionProxy>(F);
//   if (!Workgroup::isKernelToProcess(F))
//     return llvm::PreservedAnalyses::all();

//   llvm::errs() << "[SubCFG] Form SubCFGs in " << F.getName() << "\n";

//   auto &DT = AM.getResult<llvm::DominatorTreeAnalysis>(F);
//   auto &PDT = AM.getResult<llvm::PostDominatorTreeAnalysis>(F);
//   auto &LI = AM.getResult<llvm::LoopAnalysis>(F);

//   if (Workgroup::hasWorkgroupBarriers(F))
//     formSubCfgs(F, LI, DT, PDT);
//   else
//     createLoopsAroundKernel(F, DT, LI, PDT);

//   llvm::PreservedAnalyses PA;
//   PA.preserve<SplitterAnnotationAnalysis>();
//   return PA;
// }
} // namespace pocl