# Third-party packages for Tootega AIAD, found once at the top level so that their imported targets
# are visible to every directory (imported targets are directory-scoped).
include_guard(GLOBAL)

if(NOT DEFINED AIAD_DEPS)
  set(AIAD_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../deps")
endif()
cmake_path(NORMAL_PATH AIAD_DEPS)

# oneTBB 2021.13, static (the one OpenUSD was built with). Forced: Aurora's externals carry a
# DLL build of TBB that must not be picked (rule D18).
set(TBB_DIR "${AIAD_DEPS}/static/release/lib/cmake/TBB" CACHE PATH "" FORCE)

# OpenUSD 25.11, static.
find_package(pxr CONFIG REQUIRED PATHS "${AIAD_DEPS}/static/release" NO_DEFAULT_PATH)

# MFEM 4.10 + CUDA (fp64). D18: static CUDA runtime; cuSPARSE/cuBLAS stay DLLs (allowed exception).
find_package(MFEM CONFIG REQUIRED PATHS "${AIAD_DEPS}/mfem/4.10-cuda13.3-v145-double/lib/cmake/mfem" NO_DEFAULT_PATH)
find_package(CUDAToolkit REQUIRED)
get_target_property(_mfem_links mfem INTERFACE_LINK_LIBRARIES)
list(TRANSFORM _mfem_links REPLACE "^CUDA::cudart$" "CUDA::cudart_static")
set_target_properties(mfem PROPERTIES INTERFACE_LINK_LIBRARIES "${_mfem_links}")

# Project Chrono 10, static (exports /arch:AVX2 and Eigen plugins: see common/EigenConfig.h).
find_package(Chrono CONFIG REQUIRED PATHS "${AIAD_DEPS}/chrono/10.0.0-static" NO_DEFAULT_PATH)
