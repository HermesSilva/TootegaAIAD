cmake_minimum_required(VERSION 3.30)
project(S8ChronoCrank LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

find_package(Chrono CONFIG REQUIRED PATHS "D:/Tootega/Source/TootegaAIAD/deps/chrono/10.0.0-static" NO_DEFAULT_PATH)

add_executable(s8_chrono_crank main.cpp)
target_link_libraries(s8_chrono_crank PRIVATE Chrono::Chrono_core)
