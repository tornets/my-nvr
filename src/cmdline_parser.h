#ifndef NVR_CMDLINE_PARSER_H
#define NVR_CMDLINE_PARSER_H

#include "config_loader.h"
#include <string>
#include <vector>

// Parse command line arguments and return Config
// This function isolates argparse usage to avoid MSVC template issues
Config parseCommandLine(int argc, char* argv[]);

#endif // NVR_CMDLINE_PARSER_H
