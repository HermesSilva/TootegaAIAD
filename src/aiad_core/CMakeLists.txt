# aiad_core - JSON, text, paths, main-thread queue. No third-party dependency besides nlohmann/json.
add_library(aiad_core STATIC Text.cpp Paths.cpp)
target_include_directories(aiad_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${AIAD_DEPS}/_src/nlohmann_json/single_include")
target_compile_definitions(aiad_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(aiad_core PRIVATE /Zi /utf-8)
target_compile_options(aiad_core PUBLIC /utf-8)
target_link_libraries(aiad_core PUBLIC bcrypt)
