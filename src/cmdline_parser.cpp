#include "cmdline_parser.h"
#include <argparse/argparse.hpp>
#include <iostream>

Config parseCommandLine(int argc, char* argv[]) {
    argparse::ArgumentParser program("nvr", "1.0");

    // Add all arguments
    program.add_argument("-c", "--config")
        .help("Configuration file path (YAML)")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("-l", "--log-level")
        .help("Log level: trace, debug, info, warn, error, critical")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--upload-url")
        .help("Upload server URL (enables upload)")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--upload-timeout")
        .help("Upload timeout in seconds")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--upload-retries")
        .help("Upload max retries")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--record-output-dir")
        .help("Recording output directory")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--record-segment-duration")
        .help("Recording segment duration (seconds)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--record-temp-dir")
        .help("Recording temporary directory for files being recorded")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--record-filename-template")
        .help("Recording filename template")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--autoclean-max-age")
        .help("Maximum age of files to keep (hours)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--autoclean-max-disk")
        .help("Maximum disk usage (GB)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--autoclean-interval")
        .help("Cleanup check interval (seconds)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("streams")
        .help("Stream configurations")
        .remaining()
        .default_value(std::vector<std::string>{})
        .nargs(0, 100);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        throw;
    }

    // Load config
    Config config = Config::getDefault();
    std::string config_path;

    try {
        config_path = program.get<std::string>("--config");
    } catch (...) {
        config_path = "";
    }

    if (!config_path.empty()) {
        if (auto loaded = Config::fromYaml(config_path)) {
            config = *loaded;
        }
    } else {
        if (auto found = Config::findConfigFile()) {
            if (auto loaded = Config::fromYaml(*found)) {
                config = *loaded;
            }
        }
    }

    // Override with command line arguments
    try {
        std::string log_level = program.get<std::string>("--log-level");
        if (!log_level.empty()) {
            config.log_level = log_level;
        }
    } catch (...) {}

    try {
        std::string upload_url = program.get<std::string>("--upload-url");
        if (!upload_url.empty()) {
            config.upload.url = upload_url;
        }
    } catch (...) {}

    try {
        int upload_timeout = program.get<int>("--upload-timeout");
        if (upload_timeout > 0) {
            config.upload.timeout_seconds = upload_timeout;
        }
    } catch (...) {}

    try {
        int upload_retries = program.get<int>("--upload-retries");
        if (upload_retries > 0) {
            config.upload.max_retries = upload_retries;
        }
    } catch (...) {}

    try {
        std::string record_dir = program.get<std::string>("--record-output-dir");
        if (!record_dir.empty()) {
            config.record.output_dir = record_dir;
        }
    } catch (...) {}

    try {
        int segment_duration = program.get<int>("--record-segment-duration");
        if (segment_duration > 0) {
            config.record.segment_duration_seconds = segment_duration;
        }
    } catch (...) {}

    try {
        std::string temp_dir = program.get<std::string>("--record-temp-dir");
        if (!temp_dir.empty()) {
            config.record.temp_dir = temp_dir;
        }
    } catch (...) {}

    try {
        std::string filename_template = program.get<std::string>("--record-filename-template");
        if (!filename_template.empty()) {
            config.record.filename_template = filename_template;
        }
    } catch (...) {}

    try {
        int max_age = program.get<int>("--autoclean-max-age");
        if (max_age > 0) {
            config.autoclean.max_age_hours = max_age;
        }
    } catch (...) {}

    try {
        int max_disk = program.get<int>("--autoclean-max-disk");
        if (max_disk > 0) {
            config.autoclean.max_disk_usage_gb = max_disk;
        }
    } catch (...) {}

    try {
        int clean_interval = program.get<int>("--autoclean-interval");
        if (clean_interval > 0) {
            config.autoclean.check_interval_seconds = clean_interval;
        }
    } catch (...) {}

    // Parse stream arguments
    try {
        auto stream_args = program.get<std::vector<std::string>>("streams");
        for (const auto& arg : stream_args) {
            if (auto stream = Config::parseStreamArgument(arg)) {
                config.mergeStream(*stream);
            }
        }
    } catch (...) {}

    return config;
}
