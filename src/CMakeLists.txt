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
)

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