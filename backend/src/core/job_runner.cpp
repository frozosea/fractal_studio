#include "job_runner.hpp"
#include "db.hpp"

#include <cctype>
#include <chrono>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <stdexcept>
#include <sstream>

namespace fs = std::filesystem;

namespace fsd {

namespace {
// Category subfolder for a product file, by its name. "" for non-visual files (json/logs/etc.)
// — those are not mirrored into the gallery.
std::string galleryCategoryForFile(const std::string& fn) {
    std::string low = fn;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto ends = [&](const char* suf) {
        const size_t n = std::char_traits<char>::length(suf);
        return low.size() >= n && low.compare(low.size() - n, n, suf) == 0;
    };
    if (ends(".mp4") || ends(".webm") || ends(".mkv"))                                  return "videos";
    if (fn == "ln_map.png")                                                             return "ln-maps";
    if (fn == "transition.png")                                                         return "transitions";
    if (fn == "start_frame.png" || fn == "end_frame.png" || fn == "final_frame.png")    return "frames";
    if (low.rfind("atlas", 0) == 0 && ends(".png"))                                     return "atlases";
    if (ends(".stl") || ends(".glb") || ends(".obj") || ends(".ply"))                   return "meshes";
    if (fn == "map.png")                                                                return "maps";
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".webp") || ends(".bmp")) return "images";
    return "";
}
} // namespace

JobRunner::JobRunner(fs::path runtimeRoot, Db* db)
    : runtimeRoot_(std::move(runtimeRoot)), db_(db) {
    fs::create_directories(runtimeRoot_ / "runs");
}

RunRecord JobRunner::createRun(const std::string& module, const std::string& paramsJson) {
    RunRecord run;
    run.status = "queued";
    for (int attempt = 0; attempt < 1000; ++attempt) {
        run.id = makeRunId();
        const fs::path outputDir = runtimeRoot_ / "runs" / run.id;
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        runs_[run.id] = run;
        runModules_[run.id] = module;
        runParams_[run.id] = paramsJson;
        runStarted_[run.id] = started;
        runCancelRequested_[run.id] = false;
        runCancelable_[run.id] = false;
    }

    if (db_) {
        RunRow row{run.id, module, run.status, paramsJson, started, 0, run.outputDir};
        std::lock_guard<std::mutex> lk(mu_);
        db_->upsertRun(row);
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(runId);
        if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
        it->second.status = status;
        outDir = it->second.outputDir;
        module = runModules_[runId];
        paramsJson = runParams_[runId];
        started = runStarted_[runId];
    }
    if (db_) {
        const long long finished = (status == "completed" || status == "failed" || status == "cancelled") ? nowUnixMs() : 0;
        RunRow row{runId, module, status, paramsJson, started, finished, outDir};
        std::lock_guard<std::mutex> lk(mu_);
        db_->upsertRun(row);
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(runId);
        if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
        it->second.artifacts.push_back(artifact);
    }
    if (db_) {
        ArtifactRow row{0, runId, artifact.kind, artifact.path, ""};
        std::lock_guard<std::mutex> lk(mu_);
        db_->insertArtifact(row);
    }

    // Mirror visual products into runtime/gallery/<category>/ so the runs folder is browsable
    // by product type (videos/ images/ maps/ ln-maps/ frames/ meshes/ …). A relative symlink —
    // non-destructive, the run dir is untouched. Best-effort: failures are ignored.
    try {
        const fs::path src(artifact.path);
        const std::string fn = src.filename().string();
        const std::string cat = galleryCategoryForFile(fn);
        if (!cat.empty()) {
            const fs::path catDir = runtimeRoot_ / "gallery" / cat;
            std::error_code ec;
            fs::create_directories(catDir, ec);
            const fs::path link = catDir / (runId + "__" + fn);
            fs::remove(link, ec);
            const fs::path target = fs::relative(src, catDir, ec);
            fs::create_symlink(target.empty() ? src : target, link, ec);
        }
    } catch (...) { /* gallery is a convenience; never fail a run over it */ }
}

void JobRunner::requestCancel(const std::string& runId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runs_.find(runId);
    if (it == runs_.end()) throw std::runtime_error("run not found: " + runId);
    runCancelRequested_[runId] = true;
}

bool JobRunner::isCancelRequested(const std::string& runId) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runCancelRequested_.find(runId);
    return it != runCancelRequested_.end() && it->second;
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
