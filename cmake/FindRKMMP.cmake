# FindRKMPP.cmake
# 查找 Rockchip MPP (Media Process Platform) 库

# 查找头文件
find_path(ROCKCHIP_MPP_INCLUDE_DIR
    NAMES rk_mpi.h mpp_frame.h mpp_mpi.h
    PATHS
        /usr/include
        /usr/local/include
        /usr/include/rockchip
        /usr/include/mpp
        $ENV{MPP_API_PATH}
    DOC "Rockchip MPP header file location"
)

# 查找库文件
find_library(ROCKCHIP_MPP_LIBRARY
    NAMES rockchip_mpp mpp rockchip_mpp_static
    PATHS
        /lib
        /usr/lib
        /usr/local/lib
        /usr/lib/aarch64-linux-gnu
        $ENV{MPP_LIB_PATH}
    DOC "Rockchip MPP library"
)

# 查找 MPP 缓冲区库
find_library(ROCKCHIP_MPP_BUFFER_LIBRARY
    NAMES rockchip_mpp_buffer mpp_buffer
    PATHS
        /lib
        /usr/lib
        /usr/local/lib
        /usr/lib/aarch64-linux-gnu
    DOC "Rockchip MPP buffer library"
)

# 设置结果变量
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RKMPP
    REQUIRED_VARS ROCKCHIP_MPP_LIBRARY ROCKCHIP_MPP_INCLUDE_DIR
)

if(RKMPP_FOUND)
    # 创建导入目标
    if(NOT TARGET rockchip::mpp)
        add_library(rockchip::mpp UNKNOWN IMPORTED)
        set_target_properties(rockchip::mpp PROPERTIES
            IMPORTED_LOCATION "${ROCKCHIP_MPP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ROCKCHIP_MPP_INCLUDE_DIR}"
        )
    endif()

    # 设置变量供其他模块使用
    set(RKMPP_LIBRARIES ${ROCKCHIP_MPP_LIBRARY})
    if(ROCKCHIP_MPP_BUFFER_LIBRARY)
        list(APPEND RKMPP_LIBRARIES ${ROCKCHIP_MPP_BUFFER_LIBRARY})
    endif()
    set(RKMPP_INCLUDE_DIRS ${ROCKCHIP_MPP_INCLUDE_DIR})

    message(STATUS "Found Rockchip MPP:")
    message(STATUS "  Library: ${ROCKCHIP_MPP_LIBRARY}")
    if(ROCKCHIP_MPP_BUFFER_LIBRARY)
        message(STATUS "  Buffer Library: ${ROCKCHIP_MPP_BUFFER_LIBRARY}")
    endif()
    message(STATUS "  Include: ${ROCKCHIP_MPP_INCLUDE_DIR}")
else()
    message(WARNING "Rockchip MPP not found (this is OK on non-RK3588 platforms)")
    message(STATUS "  Searched paths:")
    message(STATUS "    Headers: /usr/include, /usr/include/rockchip, /usr/include/mpp")
    message(STATUS "    Libraries: /lib, /usr/lib, /usr/lib/aarch64-linux-gnu")
endif()

# 保持与 CMake 变量命名的一致性
mark_as_advanced(ROCKCHIP_MPP_INCLUDE_DIR ROCKCHIP_MPP_LIBRARY ROCKCHIP_MPP_BUFFER_LIBRARY)
