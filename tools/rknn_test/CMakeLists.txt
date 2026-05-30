cmake_minimum_required(VERSION 3.12)

# RKNN 测试工具
add_executable(rknn_test
    rknn_test.cpp
    # 复用 RKNN 推理源文件
    ${CMAKE_SOURCE_DIR}/src/rknn_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/rga_preprocessor.cpp
    ${CMAKE_SOURCE_DIR}/src/dma_buffer_extractor.cpp
)

# 包含路径
target_include_directories(rknn_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
)

# 检测 RK3588 平台
if(UNIX AND NOT APPLE)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(ARCH MATCHES "aarch64")
        set(RK3588_PLATFORM TRUE)
    endif()
endif()

# RK3588 平台特定配置
if(RK3588_PLATFORM)
    target_compile_definitions(rknn_test PRIVATE ENABLE_RKNN_SMART_RECORDING)
    target_include_directories(rknn_test PRIVATE /usr/include)
    link_directories(/lib)

    find_library(RKNPU2_LIB rknnrt HINTS /lib)
    find_library(RGA_LIB rga HINTS /lib /usr/lib)
    find_library(DRM_LIB drm HINTS /lib /usr/lib)

    if(RKNPU2_LIB)
        target_link_libraries(rknn_test PRIVATE ${RKNPU2_LIB})
    endif()
    if(RGA_LIB)
        target_link_libraries(rknn_test PRIVATE ${RGA_LIB})
    endif()
    if(DRM_LIB)
        target_link_libraries(rknn_test PRIVATE ${DRM_LIB})
    endif()
endif()

# 链接库
target_link_libraries(rknn_test PRIVATE
    ffmpeg::ffmpeg
    spdlog::spdlog
)

# 安装
install(TARGETS rknn_test RUNTIME DESTINATION bin)
