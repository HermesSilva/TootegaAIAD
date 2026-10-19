# aiad.exe - Win32 + DirectX 12 + Dear ImGui (docking) shell around the Hydra viewport and the
# engine simulation.
set(IMGUI_DIR "${AIAD_DEPS}/_src/imgui")
add_library(aiad_imgui STATIC
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp" "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp" "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp" "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
    "${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${AIAD_DEPS}/_src/implot/implot.cpp" "${AIAD_DEPS}/_src/implot/implot_items.cpp")
target_include_directories(aiad_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${AIAD_DEPS}/_src/implot")
target_link_libraries(aiad_imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)

add_executable(aiad main.cpp D3DContext.cpp DocumentScene.cpp EngineStudy.cpp EngineCommands.cpp AiPanels.cpp Markdown.cpp)
target_compile_definitions(aiad PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    AIAD_USD_PLUGIN_DIR="${AIAD_DEPS}/static/release/lib/usd"
    AIAD_HDUSDBREP_PLUGIN_DIR="${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release/usd/hdUsdBrep"
    AIAD_OMNISOLID_RESOURCES_DIR="${AIAD_DEPS}/usd-brep/1.0.0-v145-static/release/usd/omniSolid/resources"
    AIAD_HDAURORA_RESOURCES_DIR="${AIAD_DEPS}/forks/aurora-4AIAD/Libraries/HdAurora/resources"
    AIAD_MODELS_DIR="${CMAKE_SOURCE_DIR}/../models")
target_include_directories(aiad PRIVATE "${AIAD_DEPS}/_externals/aurora/stb/include")
# Release with a PDB: optimized code, symbolized crash stacks.
target_compile_options(aiad PRIVATE /Zi)
target_compile_options(aiad_imgui PRIVATE /Zi)
target_link_options(aiad PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(aiad PRIVATE aiad_imgui aiad_render aiad_doc aiad_sim_mechanism aiad_sim_engine aiad_sim_magneto aiad_cmd aiad_bridge aiad_ai dbghelp credui advapi32 shell32)
# aiad-mcp.exe ships next to aiad.exe (the agent's MCP server).
add_dependencies(aiad aiad_mcp)
set_target_properties(aiad PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${AIAD_RUNTIME_DIR}")
# The self-contained Dist folder is assembled by build-dist.ps1 at the repository root.
