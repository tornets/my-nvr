configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.h
    @ONLY
)

add_executable(nvr
    main.cpp
    ipc_recorder.cpp
    nvr_manager.cpp
    config_loader.cpp
    video_uploader.cpp
    upload_progress.cpp
    application.cpp
    cmdline_parser.cpp
    version.cpp
    schedule_utils.cpp
    log.h
)

# ============================================================================
# RK3588 智能录制支持
# ============================================================================

# 检测平台
if(UNIX AND NOT APPLE)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(ARCH MATCHES "aarch64")
        set(RK3588_PLATFORM TRUE)
        message(STATUS "RK3588 platform detected")
    endif()
endif()

# RK3588 平台特定配置
if(RK3588_PLATFORM)
    # 添加智能录制源文件
    target_sources(nvr PRIVATE
        rknn_detector.cpp
        dma_buffer_extractor.cpp
        smart_recording_manager.cpp
        frame_buffer.cpp
        detection_result_cache.cpp
        rga_preprocessor.cpp
        detection_pool.cpp
    )

    # 添加 RKNN 包含路径
    # 头文件位于 /usr/include/rknn_api.h
    target_include_directories(nvr PRIVATE /usr/include)

    # 添加编译定义
    target_compile_definitions(nvr PRIVATE
        ENABLE_RKNN_SMART_RECORDING
    )

    # 链接 RKNN 相关库
    # 库文件位于 /lib/librknnrt.so
    # 添加 /lib 到库搜索路径
    link_directories(/lib)

    find_library(RKNPU2_LIB rknnrt HINTS /lib)
    find_library(MPP_LIB rockchip_mpp HINTS /lib /usr/lib)
    find_library(RGA_LIB rga HINTS /lib /usr/lib)
    find_library(DRM_LIB drm HINTS /lib /usr/lib)

    if(RKNPU2_LIB)
        target_link_libraries(nvr PRIVATE ${RKNPU2_LIB})
        message(STATUS "Found rknpu2 library")
    else()
        message(WARNING "rknpu2 library not found, smart recording may not work")
    endif()

    if(MPP_LIB)
        target_link_libraries(nvr PRIVATE ${MPP_LIB})
        message(STATUS "Found mpp library")
    else()
        message(WARNING "mpp library not found")
    endif()

    if(RGA_LIB)
        target_link_libraries(nvr PRIVATE ${RGA_LIB})
        message(STATUS "Found rga library")
    else()
        message(WARNING "rga library not found")
    endif()

    if(DRM_LIB)
        target_link_libraries(nvr PRIVATE ${DRM_LIB})
        message(STATUS "Found drm library")
    endif()

    message(STATUS "RK3588 smart recording enabled")
else()
    message(STATUS "Smart recording disabled (non-RK3588 platform)")
endif()

# Add Windows service support on Windows
if(WIN32)
    target_sources(nvr PRIVATE
        win32_service.cpp
    )
endif()

# Include generated version.h
target_include_directories(nvr PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

if(MSVC)
    target_compile_options(nvr PRIVATE /MP /wd4576)
    target_link_libraries(nvr PRIVATE advapi32)
endif()

target_link_libraries(nvr PRIVATE
    ffmpeg::ffmpeg
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    argparse::argparse
    httplib::httplib
    nlohmann_json::nlohmann_json
)

# Install rules for packaging
install(TARGETS nvr RUNTIME)

# Install additional files from project root
if(EXISTS "${CMAKE_SOURCE_DIR}/config.yaml.example")
  install(FILES ${CMAKE_SOURCE_DIR}/config.yaml.example DESTINATION .)
endif()