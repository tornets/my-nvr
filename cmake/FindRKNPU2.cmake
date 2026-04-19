# FindRKNPU2.cmake
# 查找 RKNN Runtime 库

# 查找头文件
find_path(RKNN_INCLUDE_DIR
    NAMES rknn_api.h
    PATHS
        /usr/include
        /usr/local/include
        $ENV{RKNN_API_PATH}
    DOC "RKNN API header file location"
)

# 查找库文件
find_library(RKNNRT_LIBRARY
    NAMES rknnrt
    PATHS
        /lib
        /usr/lib
        /usr/local/lib
        $ENV{RKNN_LIB_PATH}
    DOC "RKNN Runtime library"
)

# 设置结果变量
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RKNPU2
    REQUIRED_VARS RKNNRT_LIBRARY RKNN_INCLUDE_DIR
)

if(RKNPU2_FOUND)
    # 尝试获取版本信息
    find_program(STRINGS_CMD strings)
    if(STRINGS_CMD AND RKNNRT_LIBRARY)
        execute_process(
            COMMAND ${STRINGS_CMD} ${RKNNRT_LIBRARY} COMMAND grep "librknnrt version"
            OUTPUT_VARIABLE RKNN_VERSION_STRING
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(RKNN_VERSION_STRING MATCHES "librknnrt version: ([0-9.]+)")
            set(RKNN_VERSION "${CMAKE_MATCH_1}")
            message(STATUS "RKNN Runtime version: ${RKNN_VERSION}")
        endif()
    endif()

    # 创建导入目标
    if(NOT TARGET rknpu2::rknpu2)
        add_library(rknpu2::rknpu2 UNKNOWN IMPORTED)
        set_target_properties(rknpu2::rknpu2 PROPERTIES
            IMPORTED_LOCATION "${RKNNRT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${RKNN_INCLUDE_DIR}"
        )
    endif()

    # 设置变量供其他模块使用
    set(RKNPU2_LIBRARIES ${RKNNRT_LIBRARY})
    set(RKNPU2_INCLUDE_DIRS ${RKNN_INCLUDE_DIR})

    message(STATUS "Found RKNN Runtime:")
    message(STATUS "  Library: ${RKNNRT_LIBRARY}")
    message(STATUS "  Include: ${RKNN_INCLUDE_DIR}")
else()
    message(WARNING "RKNN Runtime not found")
    message(STATUS "  Searched paths:")
    message(STATUS "    Headers: /usr/include, $ENV{RKNN_API_PATH}")
    message(STATUS "    Libraries: /lib, /usr/lib, $ENV{RKNN_LIB_PATH}")
endif()

# 保持与 CMake 变量命名的一致性
mark_as_advanced(RKNN_INCLUDE_DIR RKNNRT_LIBRARY)
