# Hydra viewport on HdAurora. Aurora (fork aurora-4AIAD) is built as a subproject in AIAD
# integration mode: static libraries, no OpenGL.
set(EXTERNALS_ROOT "${AIAD_DEPS}/_externals/aurora" CACHE PATH "" FORCE)
set(pxr_ROOT "${AIAD_DEPS}/static/release" CACHE PATH "" FORCE)
set(AIAD_INTEGRATION ON CACHE BOOL "" FORCE)
set(ENABLE_IMAGE_PROCESSING_RESOLVER OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_USDVIEW OFF CACHE BOOL "" FORCE)
set(ENABLE_APPLICATIONS OFF CACHE BOOL "" FORCE)
# D18: the same static oneTBB as OpenUSD (TBB_DIR is forced in cmake/AiadDeps.cmake); Aurora's
# externals would bring the tbb12.dll import library.
set(TBB_ROOT "${AIAD_DEPS}/static/release" CACHE PATH "" FORCE)
add_subdirectory("${AIAD_DEPS}/forks/aurora-4AIAD" aurora)

add_library(aiad_render STATIC HydraViewport.cpp)
target_include_directories(aiad_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aiad_render PRIVATE NOMINMAX)
target_compile_options(aiad_render PRIVATE /Zi)
target_link_libraries(aiad_render PUBLIC ${PXR_LIBRARIES} PRIVATE hdAurora Aurora Foundation)

# Static archives that carry registration objects (Tf types, plugins) are linked whole into the
# final executable.
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(aiad_render INTERFACE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
target_link_options(aiad_render INTERFACE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:hdAurora>")
set(USDBREP_LIB_DIR "${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release/lib")
foreach(lib SMLib SM_API SM_API_USD BREP_SM_USD BREP_USD_DATA USD_BREP_UTILS BREP_STAGING HYDRA_USD_BREP)
    target_link_libraries(aiad_render INTERFACE "${USDBREP_LIB_DIR}/${lib}.lib")
    target_link_options(aiad_render INTERFACE "/WHOLEARCHIVE:${USDBREP_LIB_DIR}/${lib}.lib")
endforeach()

# Runtime folder prepared by Aurora (MaterialX libraries, shader compilers).
set(AIAD_RUNTIME_DIR "${CMAKE_BINARY_DIR}/aurora/bin/$<CONFIG>" PARENT_SCOPE)
