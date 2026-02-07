cmake_minimum_required(VERSION 3.27)
project(NVR VERSION 1.0.0 LANGUAGES CXX)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ffmpeg REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(argparse REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)

add_subdirectory(src)

# Project metadata for packaging
set(PROJECT_DISPLAY_NAME "NVR Service")
set(PROJECT_PUBLISHER "Your Company")
set(PROJECT_URL "https://github.com/yourusername/nvr")
set(PROJECT_SERVICE_NAME "NVRService")
set(PROJECT_SERVICE_DISPLAY_NAME "NVR Video Recorder Service")

# Add packaging subdirectory (only when building from source root)
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
  add_subdirectory(cmake.packaging)
endif()