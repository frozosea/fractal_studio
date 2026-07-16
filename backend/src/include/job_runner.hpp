#pragma once

#include "types.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fsd {

class Db;

class JobRunner {
public:
    // Takes an optional persistent Db pointer. When set, run state is written
    // through to sqlite on every transition so runs survive a restart.
    explicit JobRunner(std::filesystem::path runtimeRoot, Db* db = nullptr);
    ~JobRunner();

    RunRecord createRun(const std::string& module, const std::string& paramsJson = "");
    RunRecord getRun(const std::string& runId) const;
    void setStatus(const std::string& runId, const std::string& status);
    void setProgress(const std::string& runId, const std::string& progressJson);
    std::string getProgress(const std::string& runId) const;
    void addArtifact(const std::string& runId, const Artifact& artifact);
    CancelRequestResult requestCancel(const std::string& runId);
    bool isCancelRequested(const std::string& runId) const;
    // Stable token for tight compute loops. It may be polled concurrently
    // without taking JobRunner's metadata mutex.
    std::shared_ptr<const std::atomic<bool>> cancelToken(const std::string& runId) const;
    // Keeps this runner alive at the API-lifetime boundary: the destructor
    // waits until every returned token has been released by its worker.
    std::shared_ptr<void> backgroundTaskToken();
    void setCancelable(const std::string& runId, bool cancelable);
    std::vector<ActiveTaskSnapshot> activeTasks() const;
    long long runElapsedMs(const std::string& runId) const;

    // Lookup the on-disk path for a run by id, consulting sqlite on miss.
    std::string resolveOutputDir(const std::string& runId) const;

private:
    std::filesystem::path runtimeRoot_;
    Db* db_ = nullptr;
    int runtimeLockFd_ = -1;
    std::mutex backgroundMu_;
    std::condition_variable backgroundCv_;
    size_t backgroundTaskCount_ = 0;
    bool shuttingDown_ = false;
    mutable std::mutex mu_;
    std::unordered_map<std::string, RunRecord> runs_;
    std::unordered_map<std::string, std::string> runModules_;
    std::unordered_map<std::string, std::string> runParams_;
    std::unordered_map<std::string, std::string> runProgress_;
    std::unordered_map<std::string, long long>   runStarted_;
    std::unordered_map<std::string, bool>        runCancelRequested_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> runCancelTokens_;
    std::unordered_map<std::string, bool>        runCancelable_;

    static std::string makeRunId();
};

} // namespace fsd
