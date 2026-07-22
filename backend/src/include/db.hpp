#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fsd {

struct SpecialPointRecord {
    std::string id;
    std::string family;
    std::string pointType;
    int k;
    int p;
    double re;
    double im;
    std::string sourceMode;
    std::string createdAt;
};

struct RunRow {
    std::string id;
    std::string module;
    std::string status;
    std::string paramsJson;
    long long startedAt;   // unix ms
    long long finishedAt;  // unix ms, 0 if unfinished
    std::string outputDir;
};

struct ArtifactRow {
    long long rowId;
    std::string runId;
    std::string kind;
    std::string path;
    std::string metaJson;
};

// User-compiled custom fractal variant.
struct CustomVariantRecord {
    std::string hash;      // FNV-1a hex of formula+bailout — also the DB primary key
    std::string name;      // human-readable label
    std::string formula;   // the step formula (e.g. "sin(z)*z + c")
    double      bailout;   // escape radius
    std::string soPath;    // absolute path to the compiled .so
    std::string createdAt; // ISO-8601 UTC
};

class Db {
public:
    explicit Db(std::filesystem::path dbPath);

    void ensureSchema() const;

    // Special points
    void insertSpecialPoint(const SpecialPointRecord& record) const;
    std::vector<SpecialPointRecord> listSpecialPoints(const std::string& familyFilter, int kFilter, int pFilter) const;

    // Runs + artifacts
    void upsertRun(const RunRow& row) const;
    // A newly started JobRunner cannot own workers left by an earlier process.
    // Atomically move their persistent queued/running rows to a terminal state.
    int cancelInterruptedRuns(long long finishedAt) const;
    std::vector<RunRow> listRuns(int limit) const;
    std::vector<RunRow> listRuns(int limit, int offset, const std::string& moduleFilter, const std::string& statusFilter) const;
    int countRuns(const std::string& moduleFilter = "", const std::string& statusFilter = "") const;
    std::vector<std::string> distinctModules() const;
    RunRow getRun(const std::string& runId) const;

    long long insertArtifact(const ArtifactRow& row) const;
    std::vector<ArtifactRow> listArtifacts(const std::string& runId) const;
    std::vector<ArtifactRow> listArtifactsByKind(const std::string& kind, int limit) const;
    ArtifactRow getArtifactById(long long rowId) const;

    // Compute v1 request deduplication. Product idempotency remains owned by
    // Platform; this cache prevents an at-least-once outbox delivery from
    // creating a second native run after a successful Compute response.
    bool getComputeRequestResponse(const std::string& idempotencyKey, std::string& responseJson) const;
    void upsertComputeRequestResponse(const std::string& idempotencyKey,
                                      const std::string& responseJson,
                                      long long completedAt) const;

    // Custom variants
    void insertCustomVariant(const CustomVariantRecord& r) const;
    std::vector<CustomVariantRecord> listCustomVariants() const;
    bool getCustomVariantByHash(const std::string& hash, CustomVariantRecord& out) const;
    void deleteCustomVariant(const std::string& hash) const;

private:
    std::filesystem::path dbPath_;
};

std::string nowIso8601();
long long nowUnixMs();
std::string makeId();

} // namespace fsd
