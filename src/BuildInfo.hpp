#pragma once

// What this build is. CMake generates the definitions into BuildInfo.cpp
// (from BuildInfo.cpp.in) at configure time, so a new commit recompiles
// that one file and every target links it.

#include <string>

namespace quaddeck {

// PROJECT_VERSION, the only identity a server is shown.
extern const char kBuildVersion[];
// The commit the build came from, "-dirty" when tracked files differed from
// it, "unknown" without git. For QuadDeck.log only, never a request.
extern const char kBuildCommit[];
// The configuration it was built in: Release, Debug.
extern const char kBuildConfiguration[];

// How QuadDeck's HTTP requests name their client.
inline std::string buildUserAgent() { return std::string("QuadDeck/") + kBuildVersion; }

inline std::wstring buildUserAgentWide() {
    const std::string agent = buildUserAgent();
    return std::wstring(agent.begin(), agent.end());
}

}  // namespace quaddeck
