cmake_minimum_required(VERSION 3.30)
project(S2StaticUsd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

find_package(pxr CONFIG REQUIRED)

add_executable(s2_static_usd main.cpp)
target_compile_definitions(s2_static_usd PRIVATE NOMINMAX TBB_USE_DEBUG=0)
# Static USD: every library must be linked as a whole archive so that the static registration
# objects (TfType, plugin and adapter registries) are kept by the linker.
# Only the USD archives get /WHOLEARCHIVE; their dependencies (TBB, MaterialX, system libraries)
# are linked normally.
target_link_libraries(s2_static_usd PRIVATE ${PXR_LIBRARIES})
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(s2_static_usd PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
