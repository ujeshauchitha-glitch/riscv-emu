cmake_minimum_required(VERSION 3.16)
project(riscv_emu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

add_executable(riscv_emu src/main.cpp)

target_include_directories(riscv_emu PRIVATE include)