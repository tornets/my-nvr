#include "version.h"
#include <sstream>
#include <iomanip>
#include <cstring>

namespace NVR {
    std::string getFullVersionString() {
        return VERSION_STRING;
    }

    std::string getDetailedVersionString() {
        std::ostringstream oss;
        oss << VERSION_STRING;

        if (strlen(GIT_COMMIT_HASH) > 0 && std::string(GIT_COMMIT_HASH) != "unknown") {
            oss << " (git: " << GIT_COMMIT_HASH;
            if (strlen(GIT_BRANCH) > 0 && std::string(GIT_BRANCH) != "unknown") {
                oss << ", branch: " << GIT_BRANCH;
            }
            oss << ")";
        }

        return oss.str();
    }

    std::string getBuildDateString() {
        return BUILD_DATE;
    }

    std::string getBuildTypeString() {
        return BUILD_TYPE;
    }
}
