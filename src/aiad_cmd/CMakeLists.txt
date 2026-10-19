# aiad_cmd - Command Bus: registry (JSON Schema + flags), validation, undo/redo, audit.
add_library(aiad_cmd STATIC CommandBus.cpp)
target_include_directories(aiad_cmd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(aiad_cmd PRIVATE /Zi)
target_link_libraries(aiad_cmd PUBLIC aiad_core)

add_executable(command_bus_test tests/command_bus_test.cpp)
target_link_libraries(command_bus_test PRIVATE aiad_cmd)
add_test(NAME command_bus_test COMMAND command_bus_test)
