# aiad-mcp.exe - MCP stdio server, proxy to the app's bridge. Self-contained (static, no USD/ImGui).
add_executable(aiad_mcp main.cpp)
set_target_properties(aiad_mcp PROPERTIES OUTPUT_NAME "aiad-mcp" RUNTIME_OUTPUT_DIRECTORY "${AIAD_RUNTIME_DIR}")
target_compile_options(aiad_mcp PRIVATE /Zi)
target_link_options(aiad_mcp PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(aiad_mcp PRIVATE aiad_bridge)

add_executable(mcp_bridge_test tests/mcp_bridge_test.cpp)
target_link_libraries(mcp_bridge_test PRIVATE aiad_bridge aiad_cmd aiad_ai)
target_compile_definitions(mcp_bridge_test PRIVATE AIAD_MCP_EXE="$<TARGET_FILE:aiad_mcp>")
add_dependencies(mcp_bridge_test aiad_mcp)
add_test(NAME mcp_bridge_test COMMAND mcp_bridge_test)
