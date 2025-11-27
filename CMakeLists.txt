cmake_minimum_required(VERSION 3.15)
project(LatencyTester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(LatencyTester WIN32 main.cpp)

target_link_libraries(LatencyTester PRIVATE
    d3d11
    dxgi
    d2d1
    dwrite
)

# Release build optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(LatencyTester PRIVATE /O2 /GL)
    target_link_options(LatencyTester PRIVATE /LTCG)
endif()
