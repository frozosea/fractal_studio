#include "http_server.hpp"
#include "job_runner.hpp"
#include "routes.hpp"
#include "api/routes_common.hpp"
#include "db.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <chrono>
#include <cstdlib>
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

enum class StartupBenchmarkMode {
    Off,
    Quick,
    Full,
};

StartupBenchmarkMode startup_benchmark_mode() {
    const char* raw = std::getenv("FSD_STARTUP_BENCHMARK");
    if (raw == nullptr || *raw == '\0') return StartupBenchmarkMode::Quick;

    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "off" || value == "false" || value == "0") return StartupBenchmarkMode::Off;
    if (value == "quick") return StartupBenchmarkMode::Quick;
    if (value == "full") return StartupBenchmarkMode::Full;

    std::cerr << "[startup benchmark] warning: invalid FSD_STARTUP_BENCHMARK='"
              << raw << "'; using quick" << std::endl;
    return StartupBenchmarkMode::Quick;
}

const char* startup_benchmark_mode_name(StartupBenchmarkMode mode) {
    switch (mode) {
        case StartupBenchmarkMode::Off: return "off";
        case StartupBenchmarkMode::Quick: return "quick";
        case StartupBenchmarkMode::Full: return "full";
    }
    return "quick";
}

struct StartupBenchmarkProfile {
    const char* workload;
    int width;
    int height;
    int iterations;
    bool replaceCache;
};

bool run_startup_benchmark_profile(
    fsd::JobRunner& runner,
    const StartupBenchmarkProfile& profile,
    int warmup,
    int samples
) {
    const auto started = std::chrono::steady_clock::now();
    try {
        const fsd::Json request = {
            {"centerRe", -0.75},
            {"centerIm", 0.0},
            {"scale", 1.5},
            {"width", profile.width},
            {"height", profile.height},
            {"iterations", profile.iterations},
            {"warmup", warmup},
            {"samples", samples},
            {"workload", profile.workload},
            {"replaceCache", profile.replaceCache},
        };
        const fsd::Json response = fsd::Json::parse(fsd::benchmarkRoute(runner, request.dump()));
        int available = 0;
        const auto results = response.find("results");
        if (results != response.end() && results->is_array()) {
            for (const auto& result : *results) {
                if (result.value("available", false)) ++available;
            }
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (available == 0) {
            std::cerr << "[startup benchmark] warning: workload=" << profile.workload
                      << " produced no usable engine result after " << elapsedMs
                      << " ms" << std::endl;
            return false;
        }
        std::cout << "[startup benchmark] workload=" << profile.workload
                  << " completed in " << elapsedMs << " ms ("
                  << available << " engine/scalar results)" << std::endl;
        return true;
    } catch (const std::exception& ex) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::cerr << "[startup benchmark] warning: workload=" << profile.workload
                  << " failed after " << elapsedMs << " ms: " << ex.what()
                  << std::endl;
        return false;
    } catch (...) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::cerr << "[startup benchmark] warning: workload=" << profile.workload
                  << " failed after " << elapsedMs << " ms: unknown error"
                  << std::endl;
        return false;
    }
}

void run_startup_benchmarks(fsd::JobRunner& runner, StartupBenchmarkMode mode) {
    if (mode == StartupBenchmarkMode::Off) {
        std::cout << "[startup benchmark] disabled; scheduler will use capability fallback"
                  << std::endl;
        return;
    }

    const int warmup = 1;
    const int samples = mode == StartupBenchmarkMode::Full ? 3 : 2;
    const StartupBenchmarkProfile profiles[] = {
        {"interactive", 256, 256, 1000, true},
        {"batch", 512, 512, 2000, false},
    };

    std::cout << "[startup benchmark] mode=" << startup_benchmark_mode_name(mode)
              << ", warmup=" << warmup << ", samples=" << samples
              << "; calibrating before HTTP listen" << std::endl;
    const auto started = std::chrono::steady_clock::now();
    int completed = 0;
    for (const auto& profile : profiles) {
        if (run_startup_benchmark_profile(runner, profile, warmup, samples)) ++completed;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    if (completed == 2) {
        std::cout << "[startup benchmark] calibration completed in " << elapsedMs
                  << " ms; scheduler reference is ready" << std::endl;
    } else if (completed > 0) {
        std::cerr << "[startup benchmark] warning: partial calibration completed in "
                  << elapsedMs << " ms; missing workloads will use capability fallback"
                  << std::endl;
    } else {
        std::cerr << "[startup benchmark] warning: calibration unavailable after "
                  << elapsedMs << " ms; continuing service with capability fallback"
                  << std::endl;
    }
}

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

    // Service mode is calibrated synchronously so the first HTTP request can
    // use measured scheduling data. CLI exports return above and skip this.
    run_startup_benchmarks(runner, startup_benchmark_mode());

    std::cout << "fractal_studio backend ready (native compute)" << std::endl;
    std::cout << "health: " << fsd::systemCheckRoute() << std::endl;

    fsd::HttpServer server(port, runner, repoRoot);
    server.serveForever();
    return 0;
}
