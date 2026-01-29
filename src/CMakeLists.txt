add_executable(nvr
    main.cpp
    ipc_recorder.cpp
    nvr_manager.cpp
    config_loader.cpp
    video_uploader.cpp
    application.cpp
    cmdline_parser.cpp
)

# Add Windows service support on Windows
if(WIN32)
    target_sources(nvr PRIVATE
        win32_service.cpp
    )
endif()

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
)

add_executable(transcode_aac
    transcode_aac.c
)

target_link_libraries(transcode_aac
    ffmpeg::ffmpeg
)
