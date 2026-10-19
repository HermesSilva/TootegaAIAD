# aiad_kernel - the only module that includes USD BRep / SMLib (facade over generic CAD operations).
set(USDBREP_ROOT "${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release")
add_library(aiad_kernel STATIC Kernel.cpp MeshSection.cpp)
target_include_directories(aiad_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(aiad_kernel PRIVATE
    "${USDBREP_ROOT}/include/SMLib" "${USDBREP_ROOT}/include/SM_API" "${USDBREP_ROOT}/include/SM_API_USD"
    "${USDBREP_ROOT}/include/BREP_SM_USD" "${USDBREP_ROOT}/include/BREP_USD_DATA" "${USDBREP_ROOT}/include")
# SMLib headers are not C++20-clean (ambiguous rewritten operator==): this module is C++17.
set_target_properties(aiad_kernel PROPERTIES CXX_STANDARD 17)
target_compile_definitions(aiad_kernel PRIVATE SM_STATIC_LINK SM_GRAPHICS_CALLBACKS=1 UNICODE _UNICODE NOMINMAX)
target_compile_options(aiad_kernel PRIVATE /Zi /bigobj)
target_link_libraries(aiad_kernel PUBLIC ${PXR_LIBRARIES})
foreach(lib SMLib SM_API SM_API_USD BREP_SM_USD BREP_USD_DATA USD_BREP_UTILS BREP_STAGING)
    target_link_libraries(aiad_kernel PUBLIC "${USDBREP_ROOT}/lib/${lib}.lib")
endforeach()

add_executable(kernel_test tests/kernel_test.cpp)
target_link_libraries(kernel_test PRIVATE aiad_kernel)
target_link_options(kernel_test PRIVATE /DEBUG)
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(kernel_test PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
target_compile_definitions(kernel_test PRIVATE AIAD_OMNISOLID_DIR="${USDBREP_ROOT}/usd/omniSolid/resources"
    AIAD_USD_PLUGIN_DIR="${AIAD_DEPS}/static/release/lib/usd")
add_test(NAME kernel_test COMMAND kernel_test)
