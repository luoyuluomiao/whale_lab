cmake_minimum_required(VERSION 3.20)

project(whale_song LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(whale_song src/main.cpp)

if(MSVC)
    target_compile_options(whale_song PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件: " CACHE STRING "" FORCE)
else()
    target_compile_options(whale_song PRIVATE -Wall -Wextra -Wpedantic)
endif()
