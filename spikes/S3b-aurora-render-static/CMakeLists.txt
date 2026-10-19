cmake_minimum_required(VERSION 3.30)
project(S3bAuroraRenderStatic LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

set(AIAD_DEPS "D:/Tootega/Source/TootegaAIAD/deps")

# Aurora fork as a subproject, in AIAD integration mode (static, no OpenGL).
set(EXTERNALS_ROOT "${AIAD_DEPS}/_externals/aurora" CACHE PATH "" FORCE)
set(pxr_ROOT "${AIAD_DEPS}/static/release" CACHE PATH "" FORCE)
set(AIAD_INTEGRATION ON CACHE BOOL "" FORCE)
set(ENABLE_IMAGE_PROCESSING_RESOLVER OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_USDVIEW OFF CACHE BOOL "" FORCE)
set(ENABLE_APPLICATIONS OFF CACHE BOOL "" FORCE)
add_subdirectory("${AIAD_DEPS}/forks/aurora-4AIAD" aurora)

find_package(pxr CONFIG REQUIRED)

set(USDBREP_LIB_DIR "${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release/lib")
set(USDBREP_LIBS SMLib SM_API SM_API_USD BREP_SM_USD BREP_USD_DATA USD_BREP_UTILS BREP_STAGING HYDRA_USD_BREP)

add_executable(s3b_aurora main.cpp)
target_compile_definitions(s3b_aurora PRIVATE NOMINMAX)
target_include_directories(s3b_aurora PRIVATE "${AIAD_DEPS}/_externals/aurora/stb/include"
    "${AIAD_DEPS}/forks/aurora-4AIAD/Libraries/HdAurora"
    "${AIAD_DEPS}/_externals/aurora/glm")
target_link_libraries(s3b_aurora PRIVATE hdAurora Aurora Foundation ${PXR_LIBRARIES})

# Static archives carrying registration objects are linked whole.
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(s3b_aurora PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
target_link_options(s3b_aurora PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:hdAurora>")
foreach(lib IN LISTS USDBREP_LIBS)
    target_link_libraries(s3b_aurora PRIVATE "${USDBREP_LIB_DIR}/${lib}.lib")
    target_link_options(s3b_aurora PRIVATE "/WHOLEARCHIVE:${USDBREP_LIB_DIR}/${lib}.lib")
endforeach()

# Same runtime folder as Aurora, which receives the MaterialX libraries, Slang and DXC.
set_target_properties(s3b_aurora PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aurora/bin/$<CONFIG>")
