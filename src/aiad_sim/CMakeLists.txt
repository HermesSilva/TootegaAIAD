cmake_minimum_required(VERSION 3.30)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(aiad_sim LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
  enable_testing()
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/AiadDeps.cmake)

# Chrono exports /arch:AVX2 to its consumers; every translation unit that instantiates Eigen must use
# the same instruction set, or the duplicated Eigen templates differ in alignment (ODR) and crash.
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2> /Zi)
endif()

# Engine milestone (M1): 0D cylinder thermodynamics.
add_library(aiad_sim_engine STATIC engine/EngineCylinder.cpp)
target_include_directories(aiad_sim_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aiad_sim_engine PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(aiad_sim_engine PRIVATE /W4 /permissive-)
endif()

add_executable(engine_cylinder_test tests/engine_cylinder_test.cpp)
target_link_libraries(engine_cylinder_test PRIVATE aiad_sim_engine)
add_test(NAME engine_cylinder_test COMMAND engine_cylinder_test)

# Magneto ignition: 2D magnetostatics (MFEM, CPU assembly) + lumped circuit.
add_library(aiad_sim_magneto STATIC magneto/MagnetoField.cpp magneto/MagnetoIgnition.cpp)
target_include_directories(aiad_sim_magneto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Eigen only through common/EigenConfig.h (Chrono's Eigen plugins), hence Chrono's include paths.
target_link_libraries(aiad_sim_magneto PRIVATE mfem Chrono::Chrono_core)
target_compile_features(aiad_sim_magneto PUBLIC cxx_std_20)

add_executable(magneto_test tests/magneto_test.cpp)
target_link_libraries(magneto_test PRIVATE aiad_sim_magneto)
add_test(NAME magneto_test COMMAND magneto_test)

# Coupled engine: mechanism (Project Chrono, static) + cylinder + magneto.
add_library(aiad_sim_mechanism STATIC engine/EngineSimulation.cpp)
target_include_directories(aiad_sim_mechanism PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aiad_sim_mechanism PUBLIC aiad_sim_engine aiad_sim_magneto PRIVATE Chrono::Chrono_core)

add_executable(engine_run_test tests/engine_run_test.cpp)
target_link_libraries(engine_run_test PRIVATE aiad_sim_mechanism)
add_test(NAME engine_run_test COMMAND engine_run_test)
