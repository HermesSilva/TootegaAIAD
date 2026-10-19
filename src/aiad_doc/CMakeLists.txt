# aiad_doc - the document (UsdStage with parameters, materials, parts as feature histories) and its
# Command Bus commands. Geometry only through aiad_kernel.
add_library(aiad_doc STATIC Expression.cpp Features.cpp Document.cpp DocCommands.cpp)
target_include_directories(aiad_doc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(aiad_doc PRIVATE /Zi /bigobj)
target_link_libraries(aiad_doc PUBLIC aiad_kernel aiad_cmd)

add_executable(document_test tests/document_test.cpp)
target_link_libraries(document_test PRIVATE aiad_doc)
target_link_options(document_test PRIVATE /DEBUG)
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(document_test PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
set(USDBREP_ROOT "${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release")
target_compile_definitions(document_test PRIVATE AIAD_OMNISOLID_DIR="${USDBREP_ROOT}/usd/omniSolid/resources"
    AIAD_USD_PLUGIN_DIR="${AIAD_DEPS}/static/release/lib/usd")
add_test(NAME document_test COMMAND document_test)

add_executable(aiad_model_tool tools/aiad_model.cpp)
set_target_properties(aiad_model_tool PROPERTIES OUTPUT_NAME "aiad-model")
target_link_libraries(aiad_model_tool PRIVATE aiad_doc)
target_link_options(aiad_model_tool PRIVATE /DEBUG)
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(aiad_model_tool PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
target_compile_definitions(aiad_model_tool PRIVATE AIAD_OMNISOLID_DIR="${USDBREP_ROOT}/usd/omniSolid/resources"
    AIAD_USD_PLUGIN_DIR="${AIAD_DEPS}/static/release/lib/usd")
