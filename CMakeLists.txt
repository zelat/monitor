cmake_minimum_required(VERSION 3.20)
project(monitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(LIBRT rt)

add_executable(monitor main.cc)
target_link_libraries(monitor ${LIBRT})
