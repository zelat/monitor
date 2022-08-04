cmake_minimum_required(VERSION 3.20)
project(monitor CXX)

set(CMAKE_CXX_STANDARD 11)

find_library(LIBRT rt)

add_executable(monitor main.cc)
target_link_libraries(monitor ${LIBRT})