cmake_minimum_required(VERSION 3.30)
project(TootegaAIAD LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
enable_testing()

set(AIAD_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../deps")
include(cmake/AiadDeps.cmake)

add_subdirectory(aiad_core)
add_subdirectory(aiad_cmd)
add_subdirectory(aiad_bridge)
add_subdirectory(aiad_ai)
add_subdirectory(aiad_kernel)
add_subdirectory(aiad_doc)
add_subdirectory(aiad_sim)
add_subdirectory(aiad_render)
add_subdirectory(aiad_mcp)
add_subdirectory(aiad_app)

# Dependency forks changed for AIAD, in the solution's "ThirdParty" folder. Aurora is part of the
# application; libCEED (palace-4AIAD/honee-4AIAD dependency) is built only on request.
include(cmake/AiadSolution.cmake)
aiad_set_test_folder("${CMAKE_CURRENT_SOURCE_DIR}")
aiad_set_folder("${AIAD_DEPS}/forks/aurora-4AIAD" "ThirdParty/Aurora")
if(EXISTS "${AIAD_DEPS}/forks/libceed-4AIAD/CMakeLists.txt")
    # Target-level exclusion: a directory-level EXCLUDE_FROM_ALL would also drop it from the solution.
    add_subdirectory("${AIAD_DEPS}/forks/libceed-4AIAD" libceed)
    set_target_properties(ceed PROPERTIES EXCLUDE_FROM_ALL ON)
    aiad_set_folder("${AIAD_DEPS}/forks/libceed-4AIAD" "ThirdParty/libCEED")
endif()
if(EXISTS "${AIAD_DEPS}/forks/palace-4AIAD/CMakeLists.txt")
    # Palace is a superbuild (MS-MPI, flang for ARPACK, about 20 minutes): its script drives it.
    file(TO_NATIVE_PATH "${AIAD_DEPS}/_src/build_palace.bat" _palace_bat)
    add_custom_target(palace COMMAND cmd /c "${_palace_bat}" cpu USES_TERMINAL VERBATIM)
    set_target_properties(palace PROPERTIES FOLDER "ThirdParty/Palace")
endif()
