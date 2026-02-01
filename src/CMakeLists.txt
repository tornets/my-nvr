# Generate version.h from version.h.in
# Get git commit hash and branch
find_package(Git)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        OUTPUT_VARIABLE GIT_COMMIT_HASH_FULL
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    # Get first 8 characters of hash
    string(SUBSTRING "${GIT_COMMIT_HASH_FULL}" 0 8 GIT_COMMIT_HASH)

    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH "unknown")
endif()

# Get current date in ISO format
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")

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
)

# Install rules for packaging
install(TARGETS nvr RUNTIME)

# Install additional files from project root
if(EXISTS "${CMAKE_SOURCE_DIR}/config.yaml.example")
  install(FILES ${CMAKE_SOURCE_DIR}/config.yaml.example DESTINATION .)
endif()