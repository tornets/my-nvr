cmake_minimum_required(VERSION 3.27)
project(NVR)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ffmpeg REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(argparse REQUIRED)
find_package(httplib REQUIRED)

add_subdirectory(src)