cmake_minimum_required(VERSION 3.20)
project(gorillats_test CXX)

find_package(gorillats REQUIRED)

add_executable(example src/example.cpp)
target_link_libraries(example PRIVATE gorillats::cpp)

enable_testing()
add_test(NAME example COMMAND example)
