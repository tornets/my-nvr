add_executable(nvr
    main.cpp
    ipc_recorder.cpp
    nvr_manager.cpp
)

if(MSVC)
    target_compile_options(nvr PRIVATE /MP /std:c++20 /wd4576)
else()
    target_compile_features(nvr PUBLIC cxx_std_20)
endif()

target_link_libraries(nvr
    ffmpeg::ffmpeg
    spdlog::spdlog
)


add_executable(transcode_aac
    transcode_aac.c
)

target_link_libraries(transcode_aac
    ffmpeg::ffmpeg
)
