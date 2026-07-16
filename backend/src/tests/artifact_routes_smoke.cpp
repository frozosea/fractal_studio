#include "routes.hpp"

#include "third_party/nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using Json = nlohmann::json;

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

} // namespace

int main() {
    try {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        TempTree tree{fs::temp_directory_path() / ("fsd-artifact-routes-" + std::to_string(nonce))};
        const fs::path runDir = tree.root / "fractal_studio/runtime/runs/videos/run-1";
        const fs::path nested = runDir / "_segments_tmp/final frame.bin";
        fs::create_directories(nested.parent_path());
        {
            std::ofstream out(nested, std::ios::binary);
            out << "0123456789";
        }

        const fs::path outsideDir = tree.root / "outside-run";
        const fs::path outsideFile = outsideDir / "secret.bin";
        fs::create_directories(outsideDir);
        {
            std::ofstream out(outsideFile, std::ios::binary);
            out << "secret";
        }
        std::error_code symlinkError;
        fs::create_directory_symlink(
            outsideDir,
            tree.root / "fractal_studio/runtime/runs/videos/run-link",
            symlinkError);
        require(!symlinkError, "failed to create directory symlink fixture");
        fs::create_symlink(outsideFile, runDir / "escape.bin", symlinkError);
        require(!symlinkError, "failed to create file symlink fixture");

        const Json listed = Json::parse(fsd::artifactsListRoute(tree.root, "runId=run-1"));
        require(listed.contains("items") && listed["items"].is_array(), "artifact list is missing items");
        require(listed["items"].size() == 1, "nested artifact was not listed exactly once");
        const Json& item = listed["items"][0];
        require(item.value("artifactId", std::string()) == "run-1:_segments_tmp/final frame.bin",
                "artifactId did not preserve the run-relative path");
        require(item.value("name", std::string()) == "_segments_tmp/final frame.bin",
                "artifact name did not preserve the run-relative path");
        require(item.value("contentPath", std::string()).find("%2F") != std::string::npos &&
                item.value("contentPath", std::string()).find("%20") != std::string::npos,
                "artifact contentPath was not query encoded");

        const Json allListed = Json::parse(fsd::artifactsListRoute(tree.root, ""));
        for (const Json& listedItem : allListed["items"]) {
            require(listedItem.value("runId", std::string()) != "run-link",
                    "directory symlink was listed as a run");
            require(listedItem.value("artifactId", std::string()) != "run-1:escape.bin",
                    "file symlink was listed as an artifact");
        }

        const fsd::ArtifactFile file = fsd::artifactFileRoute(
            tree.root, "artifactId=run-1%3A_segments_tmp%2Ffinal%20frame.bin");
        require(file.path == fs::canonical(nested), "nested artifact resolved to the wrong file");
        require(file.downloadName == "final frame.bin", "download name must be the basename");
        require(file.sizeBytes == 10, "artifact size is incorrect");

        bool rejectedTraversal = false;
        try {
            (void)fsd::artifactFileRoute(
                tree.root, "artifactId=run-1%3A..%2F..%2Foutside.bin");
        } catch (const std::exception&) {
            rejectedTraversal = true;
        }
        require(rejectedTraversal, "artifact path traversal was accepted");

        bool rejectedAbsolute = false;
        try {
            (void)fsd::artifactFileRoute(
                tree.root, "artifactId=run-1%3A%2Ftmp%2Foutside.bin");
        } catch (const std::exception&) {
            rejectedAbsolute = true;
        }
        require(rejectedAbsolute, "absolute artifact path was accepted");

        bool rejectedRunSymlink = false;
        try {
            (void)fsd::artifactFileRoute(
                tree.root, "artifactId=run-link%3Asecret.bin");
        } catch (const std::exception&) {
            rejectedRunSymlink = true;
        }
        require(rejectedRunSymlink, "run directory symlink escape was accepted");

        bool rejectedFileSymlink = false;
        try {
            (void)fsd::artifactFileRoute(
                tree.root, "artifactId=run-1%3Aescape.bin");
        } catch (const std::exception&) {
            rejectedFileSymlink = true;
        }
        require(rejectedFileSymlink, "artifact file symlink escape was accepted");

        bool rejectedControl = false;
        try {
            (void)fsd::artifactFileRoute(
                tree.root, "artifactId=run-1%3A_segments_tmp%2Ffinal%20frame.bin%00ignored");
        } catch (const std::exception&) {
            rejectedControl = true;
        }
        require(rejectedControl, "artifact ID containing a decoded NUL was accepted");

        std::cout << "artifact_routes_smoke: nested resolution and path guards passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "artifact_routes_smoke: " << ex.what() << '\n';
        return 1;
    }
}
