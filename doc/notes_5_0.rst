
*****************************
Release Notes for PoCL 5.0
*****************************

=============================
Major new features
=============================

~~~~~~~~~~~~~
Remote Driver
~~~~~~~~~~~~~

PoCL now has a new backend (called 'remote') for offloading OpenCL commands
across a network to one or more servers that are running the (also newly
added) 'pocld' daemon. See the `announcement <http://portablecl.org/remote-backend.html>`
and the `documentation <http://portablecl.org/docs/html/remote.html>` for more details.

=============================
Bugfixes and minor features
=============================

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
CUDA driver (partial) OpenCL 3.0 support
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

CUDA driver has gained some new features:

* program scope variables
* OpenCL 2.x/3.x atomics
* initial subgroup support (only intel_sub_group_shuffle, intel_sub_group_shuffle_xor,
  get_sub_group_local_id, sub_group_barrier, sub_group_ballot are supported)
* enable FP16 & generic address space support (with SPIR-V input)
* cl_ext_float_atomics (on fp32 + fp64) when using LLVM 17

CPU driver new features:

* cl_ext_float_atomics has been implemented (with support for fp32 & fp64)
* cl_khr_command_buffer has been updated to 0.9.4

Other:

* PoCL currently reports `cl_khr_spir` for SPIR 1.x/2.0 support, but this has
  never been tested properly and will be removed in the next release. SPIR-V
  remains the supported option.
* AlmaIF: Add DBDevice backend, which can be used to transparently
  reconfigure FPGAs from different vendors using a database of bitstreams.
  The database with the bitstreams is generated by AFOCL project
  (github.com/cpc/AFOCL). See a following publication for more info:
  Topi Leppänen, Joonas Multanen, Leevi Leppänen, Pekka Jääskeläinen:
  "AFOCL: Portable OpenCL Programming of FPGAs via Automated
   Built-in Kernel Management",
   2023 IEEE Nordic Circuits and Systems Conference (NorCAS),
   Aalborg, Denmark, 2023, pp. 1-7,
   doi: 10.1109/NorCAS58970.2023.10305457

=============================
Deprecation notice
=============================

Support for LLVM versions 10 to 13 inclusive is deprecated and will be removed in next PoCL release.

================
Acknowledgements
================

Customized Parallel Computing (CPC) research group of Tampere University,
Finland leads the development of PoCL on the side and for the needs of
their research projects. This project has received funding from the Business
Finland's AISA Veturi project. The financial support is very much appreciated
-- it keeps this open source project going!
