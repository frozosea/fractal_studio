#pragma once

#include <string>
#include <vector>

namespace fsd {

struct Artifact {
    std::string module;
    std::string path;
    std::string kind;
};

struct RunRecord {
    std::string id;
    std::string status;
    std::string outputDir;
    std::vector<Artifact> artifacts;
};

struct ActiveTaskSnapshot {
    std::string runId;
    std::string taskType;
    std::string status;
    std::string stage;
    std::string engine;
    std::string scalar;
    std::string progressJson;
    long long startedAt = 0;
    long long elapsedMs = 0;
    bool cancelable = false;
    bool cancelRequested = false;
};

struct CancelRequestResult {
    std::string runStatus;
    bool cancelable = false;
    bool cancelRequested = false;
    bool accepted = false;
};

} // namespace fsd
