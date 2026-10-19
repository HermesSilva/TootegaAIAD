cmake_minimum_required(VERSION 3.30)
project(S4bD3D12CudaInterop LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_ARCHITECTURES 75)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
# Static CUDA runtime: no cudart DLL next to the executable (D18).
set(CMAKE_CUDA_RUNTIME_LIBRARY Static)

add_executable(s4b_interop interop.cu)
target_compile_options(s4b_interop PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>)
target_link_libraries(s4b_interop PRIVATE d3d12 dxgi)
