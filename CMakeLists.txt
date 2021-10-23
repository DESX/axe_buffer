set(CMAKE_C_COMPILER_WORKS 1)

project(axe_buffer)

cmake_minimum_required(VERSION 3.5)

add_executable(axe_buffer_test tests.cpp)

target_link_libraries(axe_buffer_test fmtd Catch2 Catch2Main)

set_property(TARGET axe_buffer_test PROPERTY CXX_STANDARD 20)