#pragma once

#include "job_runner.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace fsd {

class HttpError : public std::runtime_error {
public:
    HttpError(int status, std::string body)
        : std::runtime_error(body), status_(status), body_(std::move(body)) {}
    int status() const noexcept { return status_; }
    const std::string& body() const noexcept { return body_; }

private:
    int status_ = 500;
    std::string body_;
};

// System / hardware
std::string systemCheckRoute();
std::string systemHardwareRoute();
std::string systemCapabilitiesRoute();

// Map (native): escape/metric/transition all dispatch through here.
// Artifact route returns JSON; inline route returns binary frame bytes.
std::string mapRenderRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
std::string mapRenderInlineRoute(const std::filesystem::path& repoRoot,
                                 const std::string& body,
                                 int& status,
                                 std::string& contentType,
                                 std::string& extraHeaders);
std::string mapPreemptRoute(const std::string& body);

// Raw field data (no colorization) — high-frequency tile endpoint, no artifact storage.
std::string mapFieldRoute(const std::filesystem::path& repoRoot, const std::string& body);

// Interactive raw-field sessions. A session keeps one native-resolution
// FieldOutput alive while the browser optionally presents its completed tiles
// at a lower display resolution after a latency threshold.
std::string mapFieldSessionStartRoute(const std::filesystem::path& repoRoot, const std::string& body);
std::string mapFieldSessionStatusRoute(const std::string& body);
std::string mapFieldSessionSnapshotRoute(const std::string& body);
std::string mapFieldSessionResultRoute(const std::string& body);
std::string mapFieldSessionAcknowledgeRoute(const std::string& body);

// ln-map renderer (Phase 1 ships this; Phase 2 adds the video exporter that
// consumes its output).
std::string lnMapRenderRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);

// Video export (ln-map → mp4). Real implementation in Phase 2.
std::string zoomVideoRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);

// Unified export: renders ln-map + final frame + zoom video in one request.
// Julia-aware: when julia=true, ln-map samples z₀ space with fixed c.
std::string videoExportRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
std::string videoPreviewRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);

// Transition video: theta sweep rendering (frame-by-frame, no ln-map).
std::string transitionVideoExportRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
std::string transitionVideoPreviewRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);

// HS heightfield mesh + transition 3D mesh.
std::string hsMeshRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
// HS raw field data (float64[W*H]) for frontend-rendered height mesh.
std::string hsFieldRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
std::string transitionMeshRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
// Voxel grid (Minecraft-style) for the transition renderer.
std::string transitionVoxelsRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);

// Special points (native Newton solver).
std::string specialPointsAutoRoute(const std::filesystem::path& repoRoot, const std::string& body);
std::string specialPointsSeedRoute(const std::filesystem::path& repoRoot, const std::string& body);
std::string specialPointsListRoute(const std::filesystem::path& repoRoot, const std::string& query);
std::string specialPointsEnumerateRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
std::string specialPointsSearchRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body);
std::string specialPointsResultsRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& query);
std::string specialPointsSnapRoute(const std::string& body);

// Benchmark
std::string benchmarkRoute(JobRunner& runner, const std::string& body);

// Custom variants (dynamic formula compile via g++ + dlopen)
std::string variantCompileRoute(const std::filesystem::path& repoRoot, const std::string& body);
std::string variantListRoute(const std::filesystem::path& repoRoot);
std::string variantDeleteRoute(const std::filesystem::path& repoRoot, const std::string& body);

// Registry lookup — returns the step_fn pointer for a compiled custom variant.
// Returns nullptr if the hash is unknown or compilation failed.
// Called from routes_map.cpp when variant string starts with "custom:".
struct CustomVariantLease {
    std::shared_ptr<void> library;
    void* function = nullptr;
    double bailout = 0.0;
    double bailoutSq = 0.0;
};

// Keeps the custom shared object loaded until the returned lease is released.
// Detached interactive sessions must hold this rather than retaining only the
// raw function pointer, because a user can delete a custom variant mid-render.
CustomVariantLease acquireCustomVariantLease(const std::filesystem::path& repoRoot, const std::string& hash);
void* lookupCustomFn(const std::filesystem::path& repoRoot, const std::string& hash);
double lookupCustomBailout(const std::filesystem::path& repoRoot, const std::string& hash);
double lookupCustomBailoutSq(const std::filesystem::path& repoRoot, const std::string& hash);

// Runs
std::string runsListRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& query);
std::string runStatusRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& query);
std::string activeTasksRoute(JobRunner& runner);
std::string cancelRunRoute(JobRunner& runner, const std::string& body);
std::string cancelRunRoute(JobRunner& runner, const std::string& runId, const std::string& body);

// Artifacts (filesystem scan of runs dir; artifactId = "runId:relative/path")
struct ArtifactFile {
    std::filesystem::path path;
    std::filesystem::path runDir;
    std::string contentType;
    std::string downloadName;
    std::uintmax_t sizeBytes = 0;
};

std::string artifactsListRoute(const std::filesystem::path& repoRoot, const std::string& query);
ArtifactFile artifactFileRoute(const std::filesystem::path& repoRoot, const std::string& query);

// Private, versioned Compute API used by the Platform backend.
std::string computeV1HealthRoute();
std::string computeV1CapabilitiesRoute();
std::string computeV1PreviewJsonRoute(const std::filesystem::path& repoRoot,
                                      JobRunner& runner,
                                      const std::string& body);
std::string computeV1CreateRunRoute(const std::filesystem::path& repoRoot,
                                    JobRunner& runner,
                                    const std::string& body);
std::string computeV1RunStatusRoute(const std::filesystem::path& repoRoot,
                                    JobRunner& runner,
                                    const std::string& runId);
std::string computeV1CancelRunRoute(JobRunner& runner,
                                   const std::string& runId,
                                   const std::string& body);
std::string computeV1ManifestRoute(const std::filesystem::path& repoRoot,
                                   JobRunner& runner,
                                   const std::string& runId);

} // namespace fsd
