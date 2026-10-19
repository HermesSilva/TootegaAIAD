cmake_minimum_required(VERSION 3.30)
project(S5NetgenMesh LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 17) # SMLib headers are not C++20-clean (ambiguous rewritten operator==)
set(CMAKE_CUDA_ARCHITECTURES 75)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_CUDA_RUNTIME_LIBRARY Static)

set(AIAD_DEPS "D:/Tootega/Source/TootegaAIAD/deps")
set(OCC_DIR "${AIAD_DEPS}/_build/netgen-static/dependencies/occ")
list(APPEND CMAKE_PREFIX_PATH "${AIAD_DEPS}/static/release" "${AIAD_DEPS}/_externals/aurora/MaterialX")

# Static OCCT 7.8.1 (built by Netgen's superbuild) for the nglib "occ_libs" link interface.
add_library(occ_libs INTERFACE)
foreach(tk TKDESTEP TKDEIGES TKDESTL TKXSBase TKXCAF TKVCAF TKV3d TKService TKLCAF TKCAF TKCDF TKOffset
           TKFillet TKBool TKBO TKPrim TKShHealing TKMesh TKHLR TKTopAlgo TKGeomAlgo TKBRep TKGeomBase
           TKG3d TKG2d TKMath TKernel)
    target_link_libraries(occ_libs INTERFACE "${OCC_DIR}/win64/vc14/lib/${tk}.lib")
endforeach()
target_link_libraries(occ_libs INTERFACE ws2_32 advapi32 user32)

find_package(pxr CONFIG REQUIRED PATHS "${AIAD_DEPS}/static/release" NO_DEFAULT_PATH)
find_package(Netgen CONFIG REQUIRED PATHS "${AIAD_DEPS}/netgen/6.2.2604-static/cmake" NO_DEFAULT_PATH)
find_package(MFEM CONFIG REQUIRED PATHS "${AIAD_DEPS}/mfem/4.10-cuda13.3-v145-double/lib/cmake/mfem" NO_DEFAULT_PATH)

# D18 - static only: Netgen exports the zlib import library and MFEM the shared cuSPARSE.
find_package(CUDAToolkit REQUIRED)
get_target_property(_nglib_links nglib INTERFACE_LINK_LIBRARIES)
string(REPLACE "/lib/zlib.lib" "/lib/zlibstatic.lib" _nglib_links "${_nglib_links}")
set_target_properties(nglib PROPERTIES INTERFACE_LINK_LIBRARIES "${_nglib_links}")
get_target_property(_mfem_links mfem INTERFACE_LINK_LIBRARIES)
# cuSPARSE/cuBLAS have no static libraries on Windows (NVIDIA ships them as DLLs only): D18 exception.
list(TRANSFORM _mfem_links REPLACE "^CUDA::cudart$" "CUDA::cudart_static")
set_target_properties(mfem PROPERTIES INTERFACE_LINK_LIBRARIES "${_mfem_links}")
message(STATUS "nglib links: ${_nglib_links}")
message(STATUS "mfem links: ${_mfem_links}")

set(USDBREP "${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release")
set(USDBREP_LIBS SMLib SM_API SM_API_USD BREP_SM_USD BREP_USD_DATA USD_BREP_UTILS BREP_STAGING OCCT_BREP_EXPORT OCCT_BREP_IMPORT)

add_executable(s5_netgen main.cpp)
target_compile_definitions(s5_netgen PRIVATE NOMINMAX NETGEN_STATIC SM_STATIC_LINK SM_GRAPHICS_CALLBACKS=1 UNICODE _UNICODE OCCGEOMETRY _USE_MATH_DEFINES)
target_include_directories(s5_netgen PRIVATE
    "${AIAD_DEPS}/netgen/6.2.2604-static/include"
    "${AIAD_DEPS}/netgen/6.2.2604-static/include/include"
    "${AIAD_DEPS}/netgen/6.2.2604-static/include/occ"
    "${OCC_DIR}/inc")
foreach(d SMLib SM_API SM_API_USD BREP_SM_USD BREP_USD_DATA USD_BREP_UTILS OCCT_BREP_EXPORT OCCT_BREP_IMPORT)
    target_include_directories(s5_netgen PRIVATE "${USDBREP}/include/${d}")
endforeach()
target_link_libraries(s5_netgen PRIVATE nglib ngcore occ_libs mfem ${PXR_LIBRARIES})
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(s5_netgen PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
foreach(lib IN LISTS USDBREP_LIBS)
    target_link_libraries(s5_netgen PRIVATE "${USDBREP}/lib/${lib}.lib")
endforeach()
