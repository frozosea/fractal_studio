// routes_runs.cpp — run list from the persistent runs table.

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include <filesystem>

namespace fsd {

std::string runsListRoute(const std::filesystem::path& repoRoot, const std::string& query) {
    int limit = 50;
    int offset = 0;
    const std::string limRaw = getQueryParam(query, "limit");
    if (!limRaw.empty()) { try { limit = std::stoi(limRaw); } catch (...) {} }
    const std::string offRaw = getQueryParam(query, "offset");
    if (!offRaw.empty()) { try { offset = std::stoi(offRaw); } catch (...) {} }
    // urlDecode: the module filter may be a comma-separated category list; URLSearchParams
    // percent-encodes the commas (%2C), so decode before the comma-split in Db::listRuns.
    const std::string moduleFilter = urlDecode(getQueryParam(query, "module"));
    const std::string statusFilter = urlDecode(getQueryParam(query, "status"));

    Db db = openDb(repoRoot);
    const auto rows = db.listRuns(limit, offset, moduleFilter, statusFilter);
    const int totalCount = db.countRuns(moduleFilter, statusFilter);
    const auto modules = db.distinctModules();

    Json items = Json::array();
    for (const auto& r : rows) {
        items.push_back({
            {"id",         r.id},
            {"module",     r.module},
            {"status",     r.status},
            {"startedAt",  r.startedAt},
            {"finishedAt", r.finishedAt},
            {"outputDir",  r.outputDir},
        });
    }
    Json resp = {{"items", items}, {"totalCount", totalCount}, {"modules", modules}};
    return resp.dump();
}

std::string runStatusRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& query) {
    const std::string runId = getQueryParam(query, "runId");
    if (runId.empty()) throw std::runtime_error("runId required");

    Db db = openDb(repoRoot);
    RunRow row = db.getRun(runId);
    Json progress = Json::object();
    try {
        const std::string progressText = runner.getProgress(runId);
        if (!progressText.empty()) progress = Json::parse(progressText);
    } catch (...) {
        progress = Json::object();
    }

    Json artifacts = Json::array();
    for (const auto& a : db.listArtifacts(runId)) {
        const std::filesystem::path p(a.path);
        const std::string fileName = p.filename().string();
        const std::string artifactId = runId + ":" + fileName;
        artifacts.push_back({
            {"artifactId", artifactId},
            {"name", fileName},
            {"kind", a.kind},
            {"downloadUrl", "/api/artifacts/download?artifactId=" + artifactId},
            {"contentUrl", "/api/artifacts/content?artifactId=" + artifactId},
            {"localPath", a.path},
        });
    }

    Json resp = {
        {"id", row.id},
        {"module", row.module},
        {"status", row.status},
        {"startedAt", row.startedAt},
        {"finishedAt", row.finishedAt},
        {"outputDir", row.outputDir},
        {"progress", progress},
        {"artifacts", artifacts},
    };
    return resp.dump();
}

std::string activeTasksRoute(JobRunner& runner) {
    Json items = Json::array();
    for (const auto& t : runner.activeTasks()) {
        Json progress = Json::object();
        try { progress = Json::parse(t.progressJson.empty() ? "{}" : t.progressJson); } catch (...) {}
        const std::string stage = progress.value("stage", std::string(""));
        const std::string engine = progress.value("engine", progress.value("finalFrameEngine", std::string("")));
        const std::string scalar = progress.value("scalar", progress.value("finalFrameScalar", std::string("")));
        items.push_back({
            {"runId", t.runId},
            {"taskType", progress.value("taskType", t.taskType)},
            {"status", t.status},
            {"stage", stage},
            {"engine", engine},
            {"scalar", scalar},
            {"startedAt", t.startedAt},
            {"elapsedMs", t.elapsedMs},
            {"cancelable", progress.value("cancelable", t.cancelable)},
            {"progress", progress},
        });
    }

    Json locks = Json::array();
    for (const auto& l : resourceManager().snapshot()) {
        locks.push_back({
            {"name", l.name},
            {"active", l.active},
            {"limit", l.limit},
            {"busy", l.active > 0},
            {"activeRunId", l.activeRunId},
            {"taskType", l.taskType},
        });
    }

    return Json{{"items", items}, {"resourceLocks", locks}}.dump();
}

std::string cancelRunRoute(JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string runId = j.value("runId", std::string(""));
    if (runId.empty()) throw std::runtime_error("runId required");
    return cancelRunRoute(runner, runId, body);
}

std::string cancelRunRoute(JobRunner& runner, const std::string& runId, const std::string&) {
    runner.requestCancel(runId);
    return Json{{"runId", runId}, {"status", "cancel_requested"}}.dump();
}

} // namespace fsd
