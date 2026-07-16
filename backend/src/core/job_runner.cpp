#include "job_runner.hpp"
#include "db.hpp"

#include <chrono>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <stdexcept>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace fsd {

namespace {
// Category subfolder a run is stored under (runs/<category>/<runId>/), grouped by what the run
// produces. Keep this in sync with resolveRunDirSecure() in routes_common.hpp, which finds a run in
// either this nested layout or the old flat one.
std::string categoryForModule(const std::string& m) {
    if (m == "video-export" || m == "zoom-video" || m == "video-preview" ||
        m == "transition-video-export" || m == "transition-video-preview")              return "videos";
    if (m == "ln-map")                                                                  return "ln-maps";
    if (m == "map" || m == "map-export")                                                return "maps";
    if (m == "start-frame")                                                             return "frames";
    if (m == "hs-mesh" || m == "hs-field" || m == "transition-mesh" || m == "transition-voxels") return "meshes";
    if (m == "special-points-search" || m == "special-points-enumerate")                return "points";
    if (m == "benchmark")                                                               return "benchmark";
    if (m.empty())                                                                      return "misc";
    return m;  // unknown module → its own folder
}

bool runIdExistsAnywhere(const fs::path& runtimeRoot, const std::string& runId) {
    const fs::path runsRoot = runtimeRoot / "runs";
    std::error_code ec;
    if (fs::exists(runsRoot / runId, ec)) return true;
    for (fs::directory_iterator it(runsRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        if (fs::exists(it->path() / runId, ec)) return true;
    }
    return false;
}
} // namespace

JobRunner::JobRunner(fs::path runtimeRoot, Db* db)
    : runtimeRoot_(std::move(runtimeRoot)), db_(db) {
    fs::create_directories(runtimeRoot_ / "runs");

    // Multiple CLI/backend processes may share the same runtime DB. Every live
    // JobRunner holds a shared advisory lock. A process that can first obtain
    // the exclusive lock knows there are no live runtime owners, so only that
    // process may reconcile queued/running rows from prior processes.
    const fs::path lockPath = runtimeRoot_ / ".job-runner.lock";
    const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error("failed to open JobRunner ownership lock");
    }

    const bool uniqueOwner = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (uniqueOwner) {
        try {
            if (db_) db_->cancelInterruptedRuns(nowUnixMs());
        } catch (...) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            throw;
        }
    }

    // Downgrade the unique startup lock, or join the existing owners. Keeping
    // the shared lock until destruction prevents a later CLI/backend process
    // from treating this process's active runs as interrupted.
    if (::flock(fd, LOCK_SH) != 0) {
        if (uniqueOwner) ::flock(fd, LOCK_UN);
        ::close(fd);
        throw std::runtime_error("failed to acquire JobRunner ownership lock");
    }
    runtimeLockFd_ = fd;
}

JobRunner::~JobRunner() {
    {
        std::unique_lock<std::mutex> lk(backgroundMu_);
        shuttingDown_ = true;
        backgroundCv_.wait(lk, [&] { return backgroundTaskCount_ == 0; });
    }
    if (runtimeLockFd_ >= 0) {
        ::flock(runtimeLockFd_, LOCK_UN);
        ::close(runtimeLockFd_);
    }
}

RunRecord JobRunner::createRun(const std::string& module, const std::string& paramsJson) {
    RunRecord run;
    run.status = "queued";
    // Group runs on disk by product type: runs/<category>/<runId>/. resolveRunDirSecure() finds a run
    // in this layout or the old flat runs/<runId>/.
    const fs::path categoryDir = runtimeRoot_ / "runs" / categoryForModule(module);
    {
        std::error_code ec;
        fs::create_directories(categoryDir, ec);
    }
    for (int attempt = 0; attempt < 1000; ++attempt) {
        run.id = makeRunId();
        if (runIdExistsAnywhere(runtimeRoot_, run.id)) continue;
        const fs::path outputDir = categoryDir / run.id;
        std::error_code ec;
        if (fs::create_directory(outputDir, ec)) {
            run.outputDir = outputDir.string();
            break;
        }
        if (ec) {
            throw std::runtime_error("failed to create run output directory: " + outputDir.string());
        }
    }
    if (run.outputDir.empty()) {
        throw std::runtime_error("failed to allocate unique run output directory");
    }

    const long long started = nowUnixMs();
    try {
        std::lock_guard<std::mutex> lk(mu_);
        try {
            runs_[run.id] = run;
            runModules_[run.id] = module;
            runParams_[run.id] = paramsJson;
            runStarted_[run.id] = started;
            runCancelRequested_[run.id] = false;
            runCancelTokens_[run.id] = std::make_shared<std::atomic<bool>>(false);
            runCancelable_[run.id] = false;
            if (db_) {
                RunRow row{run.id, module, run.status, paramsJson, started, 0, run.outputDir};
                db_->upsertRun(row);
            }
        } catch (...) {
            runs_.erase(run.id);
            runModules_.erase(run.id);
            runParams_.erase(run.id);
            runStarted_.erase(run.id);
            runCancelRequested_.erase(run.id);
            runCancelTokens_.erase(run.id);
            runCancelable_.erase(run.id);
            throw;
        }
    } catch (...) {
        std::error_code ec;
        fs::remove_all(run.outputDir, ec);
        throw;
    }
    return run;
}

RunRecord JobRunner::getRun(const std::string& runId) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = runs_.find(runId);
    if (it == runs_.end()) {
        throw std::runtime_error("run not found: " + runId);
    }
    return it->second;
}

void JobRunner::setStatus(const std::string& runId, const std::string& status) {
    std::string module, paramsJson, outDir;
    long long started = 0;
    const long long finished = (status == "completed" || status == "failed" || status == "cancelled") ? nowUnixMs() : 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(runId);
        if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
        it->second.status = status;
        outDir = it->second.outputDir;
        module = runModules_[runId];
        paramsJson = runParams_[runId];
        started = runStarted_[runId];
        if (db_) {
            RunRow row{runId, module, status, paramsJson, started, finished, outDir};
            db_->upsertRun(row);
        }
    }
}

void JobRunner::setProgress(const std::string& runId, const std::string& progressJson) {
    std::string outDir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(runId);
        if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
        runProgress_[runId] = progressJson;
        outDir = it->second.outputDir;
    }
    if (!outDir.empty()) {
        const fs::path finalPath = fs::path(outDir) / "progress.json";
        const fs::path tmpPath = fs::path(outDir) / "progress.json.tmp";
        {
            std::ofstream os(tmpPath);
            os << progressJson;
        }
        std::error_code ec;
        fs::rename(tmpPath, finalPath, ec);
        if (ec) {
            fs::remove(finalPath, ec);
            ec.clear();
            fs::rename(tmpPath, finalPath, ec);
        }
    }
}

std::string JobRunner::getProgress(const std::string& runId) const {
    std::string outDir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runProgress_.find(runId);
        if (it != runProgress_.end()) return it->second;
        auto runIt = runs_.find(runId);
        if (runIt != runs_.end()) outDir = runIt->second.outputDir;
    }
    if (outDir.empty()) {
        outDir = resolveOutputDir(runId);
    }
    if (!outDir.empty()) {
        std::ifstream is(fs::path(outDir) / "progress.json");
        if (is) {
            std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            if (!text.empty()) return text;
        }
    }
    return "{}";
}

void JobRunner::addArtifact(const std::string& runId, const Artifact& artifact) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runs_.find(runId);
    if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
    it->second.artifacts.push_back(artifact);
    if (db_) {
        ArtifactRow row{0, runId, artifact.kind, artifact.path, ""};
        db_->insertArtifact(row);
    }
}

void JobRunner::requestCancel(const std::string& runId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runs_.find(runId);
    if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
    runCancelRequested_[runId] = true;
    auto tokenIt = runCancelTokens_.find(runId);
    if (tokenIt != runCancelTokens_.end()) {
        tokenIt->second->store(true, std::memory_order_relaxed);
    }
}

bool JobRunner::isCancelRequested(const std::string& runId) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runCancelRequested_.find(runId);
    return it != runCancelRequested_.end() && it->second;
}

std::shared_ptr<const std::atomic<bool>> JobRunner::cancelToken(const std::string& runId) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runCancelTokens_.find(runId);
    if (it == runCancelTokens_.end()) {
        throw std::runtime_error("run not found: " + runId);
    }
    return it->second;
}

std::shared_ptr<void> JobRunner::backgroundTaskToken() {
    {
        std::lock_guard<std::mutex> lk(backgroundMu_);
        if (shuttingDown_) {
            throw std::runtime_error("JobRunner is shutting down");
        }
        ++backgroundTaskCount_;
    }
    return std::shared_ptr<void>(this, [](void* raw) {
        auto* runner = static_cast<JobRunner*>(raw);
        std::lock_guard<std::mutex> lk(runner->backgroundMu_);
        if (runner->backgroundTaskCount_ > 0) --runner->backgroundTaskCount_;
        if (runner->backgroundTaskCount_ == 0) runner->backgroundCv_.notify_all();
    });
}

void JobRunner::setCancelable(const std::string& runId, bool cancelable) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runs_.find(runId);
    if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
    runCancelable_[runId] = cancelable;
}

long long JobRunner::runElapsedMs(const std::string& runId) const {
    const long long now = nowUnixMs();
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runStarted_.find(runId);
    if (it == runStarted_.end()) return 0;
    return std::max(0LL, now - it->second);
}

std::vector<ActiveTaskSnapshot> JobRunner::activeTasks() const {
    const long long now = nowUnixMs();
    std::vector<ActiveTaskSnapshot> tasks;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [runId, run] : runs_) {
        if (run.status != "queued" && run.status != "running") continue;
        ActiveTaskSnapshot t;
        t.runId = runId;
        t.taskType = runModules_.count(runId) ? runModules_.at(runId) : "";
        t.status = run.status;
        t.progressJson = runProgress_.count(runId) ? runProgress_.at(runId) : "{}";
        t.startedAt = runStarted_.count(runId) ? runStarted_.at(runId) : 0;
        t.elapsedMs = t.startedAt > 0 ? std::max(0LL, now - t.startedAt) : 0;
        t.cancelable = runCancelable_.count(runId) ? runCancelable_.at(runId) : false;
        tasks.push_back(std::move(t));
    }
    return tasks;
}

std::string JobRunner::resolveOutputDir(const std::string& runId) const {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(runId);
        if (it != runs_.end()) return it->second.outputDir;
    }
    if (db_) {
        try {
            return db_->getRun(runId).outputDir;
        } catch (...) {}
    }
    return {};
}

std::string JobRunner::makeRunId() {
    static std::mutex idMu;
    static std::string lastStamp;
    static unsigned int sequence = 0;

    const auto now = std::chrono::system_clock::now();
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &rawTime);
#else
    localtime_r(&rawTime, &localTime);
#endif

    std::ostringstream stampStream;
    stampStream << std::put_time(&localTime, "%y%m%d-%H%M%S");
    const std::string stamp = stampStream.str();

    std::lock_guard<std::mutex> lk(idMu);
    if (stamp == lastStamp) {
        ++sequence;
    } else {
        lastStamp = stamp;
        sequence = 0;
    }

    if (sequence == 0) return stamp;

    std::ostringstream id;
    id << stamp << "-" << std::setw(3) << std::setfill('0') << sequence;
    return id.str();
}

} // namespace fsd
