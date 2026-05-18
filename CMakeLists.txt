cmake_minimum_required(VERSION 3.12)
project(NVR VERSION 1.1.7 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)

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

# IPC Scan 工具（可选）
option(BUILD_IPC_SCAN_TOOL "Build ipc_scan ONVIF device scanner tool" OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/ipc_scan/CMakeLists.txt")
  add_subdirectory(tools/ipc_scan)
endif()

# RKNN 测试工具（可选，仅 RK3588）
option(BUILD_RKNN_TEST_TOOL "Build RKNN inference test tool" OFF)
if(BUILD_RKNN_TEST_TOOL AND EXISTS "${CMAKE_SOURCE_DIR}/tools/rknn_test/CMakeLists.txt")
  add_subdirectory(tools/rknn_test)
endif()

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