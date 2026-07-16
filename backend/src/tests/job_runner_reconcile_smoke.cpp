#include "db.hpp"
#include "job_runner.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path root;
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

fsd::RunRow row(
    std::string id,
    std::string status,
    long long startedAt,
    long long finishedAt
) {
    return {
        std::move(id),
        "special-points-search",
        std::move(status),
        "{}",
        startedAt,
        finishedAt,
        "/tmp/unused",
    };
}

} // namespace

int main() {
    try {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        TempTree tree{fs::temp_directory_path() / ("fsd-job-reconcile-" + std::to_string(nonce))};
        const fs::path runtimeRoot = tree.root / "runtime";
        const fs::path dbPath = runtimeRoot / "db/fractal_studio.sqlite3";
        fs::create_directories(dbPath.parent_path());

        fsd::Db db(dbPath);
        db.ensureSchema();
        db.upsertRun(row("queued-run", "queued", 100, 0));
        db.upsertRun(row("running-run", "running", 200, 0));
        db.upsertRun(row("completed-run", "completed", 300, 400));
        db.upsertRun(row("failed-run", "failed", 500, 600));
        db.upsertRun(row("cancelled-run", "cancelled", 700, 800));

        std::string freshRunId;
        std::string peerRunId;
        std::unique_ptr<fsd::JobRunner> peer;
        {
            const long long before = fsd::nowUnixMs();
            fsd::JobRunner runner(runtimeRoot, &db);
            const long long after = fsd::nowUnixMs();

            const fsd::RunRow queued = db.getRun("queued-run");
            const fsd::RunRow running = db.getRun("running-run");
            require(queued.status == "cancelled", "queued run was not reconciled");
            require(running.status == "cancelled", "running run was not reconciled");
            require(queued.finishedAt >= before && queued.finishedAt <= after,
                    "queued run did not receive the startup finish time");
            require(running.finishedAt == queued.finishedAt,
                    "startup reconciliation was not one atomic timestamped update");

            const fsd::RunRow completed = db.getRun("completed-run");
            const fsd::RunRow failed = db.getRun("failed-run");
            const fsd::RunRow cancelled = db.getRun("cancelled-run");
            require(completed.status == "completed" && completed.finishedAt == 400,
                    "completed run was modified");
            require(failed.status == "failed" && failed.finishedAt == 600,
                    "failed run was modified");
            require(cancelled.status == "cancelled" && cancelled.finishedAt == 800,
                    "cancelled run was modified");

            require(db.cancelInterruptedRuns(fsd::nowUnixMs()) == 0,
                    "reconciliation was not idempotent");

            const fsd::RunRecord fresh = runner.createRun("special-points-search", "{}");
            freshRunId = fresh.id;
            require(db.getRun(freshRunId).status == "queued",
                    "new work was incorrectly treated as interrupted");
            const auto cancelToken = runner.cancelToken(freshRunId);
            require(!cancelToken->load(std::memory_order_relaxed),
                    "new run cancel token started requested");

            // A CLI or second backend sharing this runtime must not reconcile
            // work that is still owned by the first JobRunner.
            peer = std::make_unique<fsd::JobRunner>(runtimeRoot, &db);
            require(db.getRun(freshRunId).status == "queued",
                    "concurrent JobRunner cancelled live work owned by another process");
            peerRunId = peer->createRun("special-points-search", "{}").id;

            runner.requestCancel(freshRunId);
            require(cancelToken->load(std::memory_order_relaxed),
                    "run cancellation did not publish to the lock-free token");
        }

        // The first owner has exited, but another live JobRunner still holds a
        // shared runtime lock. A third instance must not cancel that peer's run.
        {
            fsd::JobRunner thirdRunner(runtimeRoot, &db);
            require(db.getRun(peerRunId).status == "queued",
                    "later JobRunner cancelled work owned by a live peer");
        }

        peer.reset();

        // Once every runtime owner exits, all unfinished work is orphaned and
        // the next unique owner must close it out.
        fsd::JobRunner nextOwner(runtimeRoot, &db);
        require(db.getRun(freshRunId).status == "cancelled",
                "next unique runtime owner did not reconcile orphaned work");
        require(db.getRun(peerRunId).status == "cancelled",
                "next unique runtime owner did not reconcile peer work");

        auto backgroundRunner = std::make_unique<fsd::JobRunner>(runtimeRoot, &db);
        auto backgroundToken = backgroundRunner->backgroundTaskToken();
        std::atomic<bool> workerReleased{false};
        std::thread worker([
            token = std::move(backgroundToken),
            &workerReleased
        ]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            workerReleased.store(true, std::memory_order_relaxed);
            token.reset();
        });
        backgroundRunner.reset();
        const bool destructorWaited = workerReleased.load(std::memory_order_relaxed);
        worker.join();
        require(destructorWaited,
                "JobRunner destructor did not wait for a registered background worker");

        std::cout << "job_runner_reconcile_smoke: interrupted runs reconciled\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "job_runner_reconcile_smoke: " << ex.what() << '\n';
        return 1;
    }
}
