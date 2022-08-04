cmake_minimum_required(VERSION 3.20)
project(monitor C)

set(CMAKE_C_STANDARD 99)

find_library(LIBRT rt)

add_executable(monitor main.c)
target_link_libraries(monitor ${LIBRT})