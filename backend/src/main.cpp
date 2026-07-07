#include "http_server.hpp"
#include "job_runner.hpp"
#include "routes.hpp"
#include "api/routes_common.hpp"
#include "db.hpp"

#include <csignal>
#include <chrono>
#include <cstdio>
#include <execinfo.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::filesystem::path find_studio_root(std::filesystem::path start) {
    namespace fs = std::filesystem;
    start = fs::weakly_canonical(std::move(start));
    for (fs::path probe = start; !probe.empty(); probe = probe.parent_path()) {
        if (fs::exists(probe / "backend" / "CMakeLists.txt") &&
            fs::exists(probe / "frontend" / "package.json")) {
            return probe;
        }
        if (fs::exists(probe / "fractal_studio" / "backend" / "CMakeLists.txt") &&
            fs::exists(probe / "fractal_studio" / "frontend" / "package.json")) {
            return probe / "fractal_studio";
        }
        const auto parent = probe.parent_path();
        if (parent == probe) break;
    }
    throw std::runtime_error("could not locate fractal_studio root");
}

std::string read_text_file_or_stdin(const std::string& path) {
    if (path == "-") {
        return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open JSON request: " + path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string request_body_from_cli(int argc, char* argv[], int startIndex) {
    if (argc <= startIndex) {
        throw std::runtime_error("missing JSON request path, '-' stdin, or --json <body>");
    }
    const std::string first = argv[startIndex];
    if (first == "--json") {
        if (argc <= startIndex + 1) throw std::runtime_error("missing JSON body after --json");
        return argv[startIndex + 1];
    }
    return read_text_file_or_stdin(first);
}

void print_cli_progress(const fsd::Json& progress) {
    const std::string stage = progress.value("stage", std::string(""));
    const int current = progress.value("current", 0);
    const int total = progress.value("total", 0);
    const double percent = progress.value("percent", total > 0 ? 100.0 * static_cast<double>(current) / total : 0.0);
    std::cerr << "\r" << stage << " " << current << "/" << total
              << " " << std::fixed << std::setprecision(1) << percent << "%";
    const auto eta = progress.find("estimatedRemainingMs");
    if (eta != progress.end() && eta->is_number()) {
        std::cerr << " eta " << std::setprecision(0) << (eta->get<double>() / 1000.0) << "s";
    }
    std::cerr << "      " << std::flush;
}

int run_cli_export(
    const std::string& command,
    const std::filesystem::path& repoRoot,
    fsd::JobRunner& runner,
    std::string body
) {
    fsd::Json request = fsd::parseJsonBody(body);
    request["localExport"] = true;

    if (command == "export-map") {
        request["taskType"] = "still_export";
        const std::string response = fsd::mapRenderRoute(repoRoot, runner, request.dump());
        std::cout << response << std::endl;
        return 0;
    }

    if (command == "export-video") {
        request["background"] = true;
        const fsd::Json initial = fsd::Json::parse(fsd::videoExportRoute(repoRoot, runner, request.dump()));
        const std::string runId = initial.value("runId", std::string(""));
        if (runId.empty()) {
            std::cout << initial.dump(2) << std::endl;
            return 1;
        }
        std::cerr << "runId " << runId << std::endl;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            fsd::RunRecord run = runner.getRun(runId);
            fsd::Json progress = fsd::parseJsonBody(runner.getProgress(runId));
            print_cli_progress(progress);
            if (run.status == "completed" || run.status == "failed" || run.status == "cancelled") {
                std::cerr << std::endl;
                if (run.status == "completed") {
                    fsd::Json out = initial;
                    out["status"] = "completed";
                    out["outputDir"] = run.outputDir;
                    out["artifacts"] = fsd::Json::array();
                    for (const auto& artifact : run.artifacts) {
                        out["artifacts"].push_back(fsd::Json{
                            {"module", artifact.module},
                            {"kind", artifact.kind},
                            {"path", artifact.path},
                            {"name", std::filesystem::path(artifact.path).filename().string()},
                        });
                    }
                    std::cout << out.dump(2) << std::endl;
                    return 0;
                }
                progress["runId"] = runId;
                progress["status"] = run.status;
                std::cout << progress.dump(2) << std::endl;
                return run.status == "cancelled" ? 130 : 1;
            }
        }
    }

    throw std::runtime_error("unknown CLI command: " + command);
}

} // namespace

static void crash_handler(int sig) {
    // Write backtrace to stderr (goes to the log file when redirected).
    // async-signal-safe: uses write(), not printf/cout.
    void* buf[64];
    const int n = ::backtrace(buf, 64);
    const char* msg = "=== CRASH HANDLER ===\n";
    ::write(STDERR_FILENO, msg, __builtin_strlen(msg));
    ::backtrace_symbols_fd(buf, n, STDERR_FILENO);
    // Re-raise with default handler so the core dump is still produced.
    std::signal(sig, SIG_DFL);
    ::raise(sig);
}

int main(int argc, char* argv[]) {
    // Catch segfaults and print a backtrace before dying.
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGBUS,  crash_handler);

    // Suppress SIGPIPE so that writing to a closed client socket returns -1
    // instead of crashing the process. Browsers close connections aggressively
    // (page refresh, navigation, prefetch cancellation) and ::send() would
    // otherwise deliver SIGPIPE the moment it tries to respond.
    std::signal(SIGPIPE, SIG_IGN);

    namespace fs = std::filesystem;

    const fs::path studioRoot = find_studio_root(fs::current_path());
    const fs::path repoRoot = studioRoot.parent_path();

    const fs::path runtimeRoot = studioRoot / "runtime";
    const fs::path dbDir = runtimeRoot / "db";
    fs::create_directories(dbDir);
    fsd::Db db(dbDir / "fractal_studio.sqlite3");
    db.ensureSchema();

    fsd::JobRunner runner(runtimeRoot, &db);

    if (argc > 1) {
        const std::string command = argv[1];
        if (command == "export-map" || command == "export-video") {
            try {
                return run_cli_export(command, repoRoot, runner, request_body_from_cli(argc, argv, 2));
            } catch (const std::exception& ex) {
                std::cerr << "error: " << ex.what() << std::endl;
                return 1;
            }
        }
    }

    std::cout << "fractal_studio backend ready (native compute)" << std::endl;
    std::cout << "health: " << fsd::systemCheckRoute() << std::endl;

    int port = 18080;  // frontend api.ts default
    if (argc > 1) {
        // First positional arg can be either "serve" (legacy) or a port number.
        const std::string arg1 = argv[1];
        if (arg1 == "serve") {
            if (argc > 2) port = std::stoi(argv[2]);
        } else {
            try { port = std::stoi(arg1); } catch (...) {}
        }
    }

    fsd::HttpServer server(port, runner, repoRoot);
    server.serveForever();
    return 0;
}
