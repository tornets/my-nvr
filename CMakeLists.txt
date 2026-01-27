cmake_minimum_required(VERSION 3.27)
project(NVR)

find_package(ZLIB REQUIRED)
find_package(ffmpeg REQUIRED)
find_package(spdlog REQUIRED)

add_subdirectory(src)