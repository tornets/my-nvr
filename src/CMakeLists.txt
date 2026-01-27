add_executable(test
    main.cpp
    ipc_recorder.cpp
)

target_link_libraries(test
    ffmpeg::ffmpeg
    spdlog::spdlog
)


add_executable(transcode_aac
    transcode_aac.c
)

target_link_libraries(transcode_aac
    ffmpeg::ffmpeg
)
