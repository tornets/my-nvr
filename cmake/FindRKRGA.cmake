# FindRKRGA.cmake
# 查找 Rockchip RGA (2D Graphics Acceleration) 库

# 查找头文件
find_path(ROCKCHIP_RGA_INCLUDE_DIR
    NAMES rga.h rk_rga.h librga.h
    PATHS
        /usr/include
        /usr/local/include
        /usr/include/rockchip
        /usr/include/rga
        $ENV{RGA_API_PATH}
    DOC "Rockchip RGA header file location"
)

# 查找库文件
find_library(ROCKCHIP_RGA_LIBRARY
    NAMES rga rockchip_rga rga2
    PATHS
        /lib
        /usr/lib
        /usr/local/lib
        /usr/lib/aarch64-linux-gnu
        $ENV{RGA_LIB_PATH}
    DOC "Rockchip RGA library"
)

# 设置结果变量
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RKRGA
    REQUIRED_VARS ROCKCHIP_RGA_LIBRARY ROCKCHIP_RGA_INCLUDE_DIR
)

if(RKRGA_FOUND)
    # 创建导入目标
    if(NOT TARGET rockchip::rga)
        add_library(rockchip::rga UNKNOWN IMPORTED)
        set_target_properties(rockchip::rga PROPERTIES
            IMPORTED_LOCATION "${ROCKCHIP_RGA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ROCKCHIP_RGA_INCLUDE_DIR}"
        )
    endif()

    # 设置变量供其他模块使用
    set(RKRGA_LIBRARIES ${ROCKCHIP_RGA_LIBRARY})
    set(RKRGA_INCLUDE_DIRS ${ROCKCHIP_RGA_INCLUDE_DIR})

    message(STATUS "Found Rockchip RGA:")
    message(STATUS "  Library: ${ROCKCHIP_RGA_LIBRARY}")
    message(STATUS "  Include: ${ROCKCHIP_RGA_INCLUDE_DIR}")
else()
    message(WARNING "Rockchip RGA not found (this is OK on non-RK3588 platforms)")
    message(STATUS "  Searched paths:")
    message(STATUS "    Headers: /usr/include, /usr/include/rockchip, /usr/include/rga")
    message(STATUS "    Libraries: /lib, /usr/lib, /usr/lib/aarch64-linux-gnu")
endif()

# 保持与 CMake 变量命名的一致性
mark_as_advanced(ROCKCHIP_RGA_INCLUDE_DIR ROCKCHIP_RGA_LIBRARY)
