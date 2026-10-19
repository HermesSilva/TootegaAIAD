# aiad_bridge - loopback agent bridge (server side in the app, client side in aiad-mcp.exe).
add_library(aiad_bridge STATIC Bridge.cpp)
target_include_directories(aiad_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(aiad_bridge PRIVATE /Zi)
target_link_libraries(aiad_bridge PUBLIC aiad_core PRIVATE ws2_32 winhttp)
