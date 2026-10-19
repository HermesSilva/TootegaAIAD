cmake_minimum_required(VERSION 3.30)
project(S3aHydraBrepStatic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

find_package(pxr CONFIG REQUIRED)

set(USDBREP_LIB_DIR "D:/Tootega/Source/TootegaAIAD/deps/_src/usd-brep/_build/windows-x86_64/release")
set(USDBREP_LIBS SMLib SM_API SM_API_USD BREP_SM_USD BREP_USD_DATA USD_BREP_UTILS BREP_STAGING HYDRA_USD_BREP)

add_executable(s3a_hydra_brep main.cpp)
target_compile_definitions(s3a_hydra_brep PRIVATE NOMINMAX)

# Static OpenUSD and static USD BRep: archives carrying registration objects are linked whole.
target_link_libraries(s3a_hydra_brep PRIVATE ${PXR_LIBRARIES})
foreach(lib IN LISTS PXR_LIBRARIES)
    if(TARGET ${lib})
        target_link_options(s3a_hydra_brep PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:${lib}>")
    endif()
endforeach()
foreach(lib IN LISTS USDBREP_LIBS)
    target_link_libraries(s3a_hydra_brep PRIVATE "${USDBREP_LIB_DIR}/${lib}.lib")
    target_link_options(s3a_hydra_brep PRIVATE "/WHOLEARCHIVE:${USDBREP_LIB_DIR}/${lib}.lib")
endforeach()
