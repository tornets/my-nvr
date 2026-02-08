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
