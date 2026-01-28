add_executable(nvr
    main.cpp
    ipc_recorder.cpp
    nvr_manager.cpp
    config_loader.cpp
    video_uploader.cpp
)

if(MSVC)
    target_compile_options(nvr PRIVATE /MP /wd4576)
endif()

target_link_libraries(nvr
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
