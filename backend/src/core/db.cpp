#include "db.hpp"

#include <sqlite3.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace fsd {

namespace {

void checkSqlite(int rc, sqlite3* db) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        const char* msg = db != nullptr ? sqlite3_errmsg(db) : "sqlite error";
        throw std::runtime_error(msg);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db), stmt_(nullptr) {
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        checkSqlite(rc, db_);
    }

    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_;
};

struct DbHandle {
    sqlite3* db = nullptr;
    explicit DbHandle(const std::filesystem::path& path) {
        const int rc = sqlite3_open(path.string().c_str(), &db);
        checkSqlite(rc, db);
        // WAL mode: allows concurrent reads; serialised writes with retry.
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        // Retry for up to 2 seconds if the DB is locked by another thread.
        sqlite3_busy_timeout(db, 2000);
    }
    ~DbHandle() {
        if (db) sqlite3_close(db);
    }
    DbHandle(const DbHandle&) = delete;
    DbHandle& operator=(const DbHandle&) = delete;
};

void execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

std::string safeText(sqlite3_stmt* stmt, int col) {
    const unsigned char* p = sqlite3_column_text(stmt, col);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
}

} // namespace

Db::Db(std::filesystem::path dbPath) : dbPath_(std::move(dbPath)) {}

void Db::ensureSchema() const {
    DbHandle h(dbPath_);
    execSql(h.db,
        "CREATE TABLE IF NOT EXISTS special_points ("
        "id TEXT PRIMARY KEY,"
        "family TEXT NOT NULL,"
        "point_type TEXT NOT NULL,"
        "k INTEGER NOT NULL,"
        "p INTEGER NOT NULL,"
        "re REAL NOT NULL,"
        "im REAL NOT NULL,"
        "source_mode TEXT NOT NULL,"
        "created_at TEXT NOT NULL"
        ");");

    execSql(h.db,
        "CREATE TABLE IF NOT EXISTS runs ("
        "id TEXT PRIMARY KEY,"
        "module TEXT NOT NULL,"
        "status TEXT NOT NULL,"
        "params_json TEXT NOT NULL DEFAULT '',"
        "started_at INTEGER NOT NULL,"
        "finished_at INTEGER NOT NULL DEFAULT 0,"
        "output_dir TEXT NOT NULL"
        ");");

    execSql(h.db,
        "CREATE TABLE IF NOT EXISTS artifacts ("
        "row_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "run_id TEXT NOT NULL,"
        "kind TEXT NOT NULL,"
        "path TEXT NOT NULL,"
        "meta_json TEXT NOT NULL DEFAULT ''"
        ");");

    execSql(h.db, "CREATE INDEX IF NOT EXISTS idx_artifacts_run ON artifacts(run_id);");
    execSql(h.db, "CREATE INDEX IF NOT EXISTS idx_artifacts_kind ON artifacts(kind);");

    execSql(h.db,
        "CREATE TABLE IF NOT EXISTS custom_variants ("
        "hash TEXT PRIMARY KEY,"
        "name TEXT NOT NULL,"
        "formula TEXT NOT NULL,"
        "bailout REAL NOT NULL,"
        "so_path TEXT NOT NULL,"
        "created_at TEXT NOT NULL"
        ");");
}

void Db::insertSpecialPoint(const SpecialPointRecord& record) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "INSERT INTO special_points "
        "(id, family, point_type, k, p, re, im, source_mode, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");

    checkSqlite(sqlite3_bind_text(stmt.get(), 1, record.id.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 2, record.family.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 3, record.pointType.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_int(stmt.get(), 4, record.k), h.db);
    checkSqlite(sqlite3_bind_int(stmt.get(), 5, record.p), h.db);
    checkSqlite(sqlite3_bind_double(stmt.get(), 6, record.re), h.db);
    checkSqlite(sqlite3_bind_double(stmt.get(), 7, record.im), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 8, record.sourceMode.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 9, record.createdAt.c_str(), -1, SQLITE_TRANSIENT), h.db);

    checkSqlite(sqlite3_step(stmt.get()), h.db);
}

std::vector<SpecialPointRecord> Db::listSpecialPoints(const std::string& familyFilter, int kFilter, int pFilter) const {
    DbHandle h(dbPath_);

    std::string sql =
        "SELECT id, family, point_type, k, p, re, im, source_mode, created_at "
        "FROM special_points ";

    bool hasWhere = false;
    if (!familyFilter.empty()) { sql += hasWhere ? "AND " : "WHERE "; sql += "family = ? "; hasWhere = true; }
    if (kFilter >= 0)          { sql += hasWhere ? "AND " : "WHERE "; sql += "k = ? ";      hasWhere = true; }
    if (pFilter >= 0)          { sql += hasWhere ? "AND " : "WHERE "; sql += "p = ? ";      hasWhere = true; }
    sql += "ORDER BY created_at DESC, id DESC LIMIT 500;";

    Statement stmt(h.db, sql.c_str());
    int index = 1;
    if (!familyFilter.empty()) checkSqlite(sqlite3_bind_text(stmt.get(), index++, familyFilter.c_str(), -1, SQLITE_TRANSIENT), h.db);
    if (kFilter >= 0)          checkSqlite(sqlite3_bind_int (stmt.get(), index++, kFilter), h.db);
    if (pFilter >= 0)          checkSqlite(sqlite3_bind_int (stmt.get(), index++, pFilter), h.db);

    std::vector<SpecialPointRecord> rows;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        checkSqlite(rc, h.db);
        SpecialPointRecord row;
        row.id         = safeText(stmt.get(), 0);
        row.family     = safeText(stmt.get(), 1);
        row.pointType  = safeText(stmt.get(), 2);
        row.k          = sqlite3_column_int(stmt.get(), 3);
        row.p          = sqlite3_column_int(stmt.get(), 4);
        row.re         = sqlite3_column_double(stmt.get(), 5);
        row.im         = sqlite3_column_double(stmt.get(), 6);
        row.sourceMode = safeText(stmt.get(), 7);
        row.createdAt  = safeText(stmt.get(), 8);
        rows.push_back(row);
    }
    return rows;
}

void Db::upsertRun(const RunRow& row) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "INSERT INTO runs (id, module, status, params_json, started_at, finished_at, output_dir) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  module=excluded.module,"
        "  status=excluded.status,"
        "  params_json=excluded.params_json,"
        "  started_at=excluded.started_at,"
        "  finished_at=excluded.finished_at,"
        "  output_dir=excluded.output_dir;");

    checkSqlite(sqlite3_bind_text  (stmt.get(), 1, row.id.c_str(),         -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 2, row.module.c_str(),     -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 3, row.status.c_str(),     -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 4, row.paramsJson.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_int64 (stmt.get(), 5, row.startedAt),                                h.db);
    checkSqlite(sqlite3_bind_int64 (stmt.get(), 6, row.finishedAt),                               h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 7, row.outputDir.c_str(),  -1, SQLITE_TRANSIENT), h.db);

    checkSqlite(sqlite3_step(stmt.get()), h.db);
}

std::vector<RunRow> Db::listRuns(int limit) const {
    return listRuns(limit, 0, "", "");
}

// A module filter may be a single module, or a comma-separated list (a category spans several
// modules). Empty segments are kept so the "Other" category can include the empty-module runs;
// an entirely empty filter means "no module filter". Returns {} for no filter.
static std::vector<std::string> splitModuleFilter(const std::string& f) {
    std::vector<std::string> out;
    if (f.empty()) return out;
    size_t start = 0;
    for (;;) {
        const size_t comma = f.find(',', start);
        out.push_back(f.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Builds the "module IN (?,?,…)" fragment for a (possibly multi-valued) module filter.
static std::string moduleInClause(const std::vector<std::string>& moduleVals) {
    std::string c = "module IN (";
    for (size_t i = 0; i < moduleVals.size(); ++i) c += (i ? ",?" : "?");
    return c + ")";
}

std::vector<RunRow> Db::listRuns(int limit, int offset, const std::string& moduleFilter, const std::string& statusFilter) const {
    DbHandle h(dbPath_);
    const std::vector<std::string> moduleVals = splitModuleFilter(moduleFilter);
    std::string sql = "SELECT id, module, status, params_json, started_at, finished_at, output_dir FROM runs";
    std::vector<std::string> conditions;
    if (!moduleVals.empty()) conditions.push_back(moduleInClause(moduleVals));
    if (!statusFilter.empty()) conditions.push_back("status = ?");
    for (size_t i = 0; i < conditions.size(); ++i) {
        sql += (i == 0 ? " WHERE " : " AND ") + conditions[i];
    }
    sql += " ORDER BY started_at DESC LIMIT ? OFFSET ?;";

    Statement stmt(h.db, sql.c_str());
    int idx = 1;
    for (const auto& v : moduleVals) checkSqlite(sqlite3_bind_text(stmt.get(), idx++, v.c_str(), -1, SQLITE_TRANSIENT), h.db);
    if (!statusFilter.empty()) checkSqlite(sqlite3_bind_text(stmt.get(), idx++, statusFilter.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_int(stmt.get(), idx++, limit > 0 ? limit : 200), h.db);
    checkSqlite(sqlite3_bind_int(stmt.get(), idx++, std::max(0, offset)), h.db);

    std::vector<RunRow> rows;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        checkSqlite(rc, h.db);
        RunRow r;
        r.id         = safeText(stmt.get(), 0);
        r.module     = safeText(stmt.get(), 1);
        r.status     = safeText(stmt.get(), 2);
        r.paramsJson = safeText(stmt.get(), 3);
        r.startedAt  = sqlite3_column_int64(stmt.get(), 4);
        r.finishedAt = sqlite3_column_int64(stmt.get(), 5);
        r.outputDir  = safeText(stmt.get(), 6);
        rows.push_back(r);
    }
    return rows;
}

int Db::countRuns(const std::string& moduleFilter, const std::string& statusFilter) const {
    DbHandle h(dbPath_);
    const std::vector<std::string> moduleVals = splitModuleFilter(moduleFilter);
    std::string sql = "SELECT COUNT(*) FROM runs";
    std::vector<std::string> conditions;
    if (!moduleVals.empty()) conditions.push_back(moduleInClause(moduleVals));
    if (!statusFilter.empty()) conditions.push_back("status = ?");
    for (size_t i = 0; i < conditions.size(); ++i) {
        sql += (i == 0 ? " WHERE " : " AND ") + conditions[i];
    }
    sql += ";";

    Statement stmt(h.db, sql.c_str());
    int idx = 1;
    for (const auto& v : moduleVals) checkSqlite(sqlite3_bind_text(stmt.get(), idx++, v.c_str(), -1, SQLITE_TRANSIENT), h.db);
    if (!statusFilter.empty()) checkSqlite(sqlite3_bind_text(stmt.get(), idx++, statusFilter.c_str(), -1, SQLITE_TRANSIENT), h.db);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return 0;
    checkSqlite(rc, h.db);
    return sqlite3_column_int(stmt.get(), 0);
}

std::vector<std::string> Db::distinctModules() const {
    DbHandle h(dbPath_);
    Statement stmt(h.db, "SELECT DISTINCT module FROM runs ORDER BY module;");
    std::vector<std::string> modules;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        checkSqlite(rc, h.db);
        modules.push_back(safeText(stmt.get(), 0));
    }
    return modules;
}

RunRow Db::getRun(const std::string& runId) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "SELECT id, module, status, params_json, started_at, finished_at, output_dir "
        "FROM runs WHERE id = ?;");
    checkSqlite(sqlite3_bind_text(stmt.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), h.db);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) throw std::runtime_error("run not found: " + runId);
    checkSqlite(rc, h.db);

    RunRow r;
    r.id         = safeText(stmt.get(), 0);
    r.module     = safeText(stmt.get(), 1);
    r.status     = safeText(stmt.get(), 2);
    r.paramsJson = safeText(stmt.get(), 3);
    r.startedAt  = sqlite3_column_int64(stmt.get(), 4);
    r.finishedAt = sqlite3_column_int64(stmt.get(), 5);
    r.outputDir  = safeText(stmt.get(), 6);
    return r;
}

long long Db::insertArtifact(const ArtifactRow& row) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "INSERT INTO artifacts (run_id, kind, path, meta_json) VALUES (?, ?, ?, ?);");
    checkSqlite(sqlite3_bind_text(stmt.get(), 1, row.runId.c_str(),    -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 2, row.kind.c_str(),     -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 3, row.path.c_str(),     -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text(stmt.get(), 4, row.metaJson.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_step(stmt.get()), h.db);
    return static_cast<long long>(sqlite3_last_insert_rowid(h.db));
}

std::vector<ArtifactRow> Db::listArtifacts(const std::string& runId) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "SELECT row_id, run_id, kind, path, meta_json FROM artifacts "
        "WHERE run_id = ? ORDER BY row_id ASC;");
    checkSqlite(sqlite3_bind_text(stmt.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), h.db);

    std::vector<ArtifactRow> rows;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        checkSqlite(rc, h.db);
        ArtifactRow a;
        a.rowId    = sqlite3_column_int64(stmt.get(), 0);
        a.runId    = safeText(stmt.get(), 1);
        a.kind     = safeText(stmt.get(), 2);
        a.path     = safeText(stmt.get(), 3);
        a.metaJson = safeText(stmt.get(), 4);
        rows.push_back(a);
    }
    return rows;
}

std::vector<ArtifactRow> Db::listArtifactsByKind(const std::string& kind, int limit) const {
    DbHandle h(dbPath_);
    std::string sql =
        "SELECT row_id, run_id, kind, path, meta_json FROM artifacts ";
    if (!kind.empty()) sql += "WHERE kind = ? ";
    sql += "ORDER BY row_id DESC LIMIT ?;";
    Statement stmt(h.db, sql.c_str());
    int idx = 1;
    if (!kind.empty()) checkSqlite(sqlite3_bind_text(stmt.get(), idx++, kind.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_int(stmt.get(), idx, limit > 0 ? limit : 200), h.db);

    std::vector<ArtifactRow> rows;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        checkSqlite(rc, h.db);
        ArtifactRow a;
        a.rowId    = sqlite3_column_int64(stmt.get(), 0);
        a.runId    = safeText(stmt.get(), 1);
        a.kind     = safeText(stmt.get(), 2);
        a.path     = safeText(stmt.get(), 3);
        a.metaJson = safeText(stmt.get(), 4);
        rows.push_back(a);
    }
    return rows;
}

ArtifactRow Db::getArtifactById(long long rowId) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "SELECT row_id, run_id, kind, path, meta_json FROM artifacts WHERE row_id = ?;");
    checkSqlite(sqlite3_bind_int64(stmt.get(), 1, rowId), h.db);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) throw std::runtime_error("artifact not found: " + std::to_string(rowId));
    checkSqlite(rc, h.db);
    ArtifactRow a;
    a.rowId    = sqlite3_column_int64(stmt.get(), 0);
    a.runId    = safeText(stmt.get(), 1);
    a.kind     = safeText(stmt.get(), 2);
    a.path     = safeText(stmt.get(), 3);
    a.metaJson = safeText(stmt.get(), 4);
    return a;
}

void Db::insertCustomVariant(const CustomVariantRecord& r) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "INSERT OR REPLACE INTO custom_variants (hash, name, formula, bailout, so_path, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?);");
    checkSqlite(sqlite3_bind_text  (stmt.get(), 1, r.hash.c_str(),      -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 2, r.name.c_str(),      -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 3, r.formula.c_str(),   -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_double(stmt.get(), 4, r.bailout),                                  h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 5, r.soPath.c_str(),    -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_bind_text  (stmt.get(), 6, r.createdAt.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_step(stmt.get()), h.db);
}

std::vector<CustomVariantRecord> Db::listCustomVariants() const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "SELECT hash, name, formula, bailout, so_path, created_at "
        "FROM custom_variants ORDER BY created_at DESC LIMIT 200;");
    std::vector<CustomVariantRecord> rows;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        checkSqlite(rc, h.db);
        CustomVariantRecord r;
        r.hash      = safeText(stmt.get(), 0);
        r.name      = safeText(stmt.get(), 1);
        r.formula   = safeText(stmt.get(), 2);
        r.bailout   = sqlite3_column_double(stmt.get(), 3);
        r.soPath    = safeText(stmt.get(), 4);
        r.createdAt = safeText(stmt.get(), 5);
        rows.push_back(r);
    }
    return rows;
}

bool Db::getCustomVariantByHash(const std::string& hash, CustomVariantRecord& out) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db,
        "SELECT hash, name, formula, bailout, so_path, created_at "
        "FROM custom_variants WHERE hash = ?;");
    checkSqlite(sqlite3_bind_text(stmt.get(), 1, hash.c_str(), -1, SQLITE_TRANSIENT), h.db);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return false;
    checkSqlite(rc, h.db);
    out.hash      = safeText(stmt.get(), 0);
    out.name      = safeText(stmt.get(), 1);
    out.formula   = safeText(stmt.get(), 2);
    out.bailout   = sqlite3_column_double(stmt.get(), 3);
    out.soPath    = safeText(stmt.get(), 4);
    out.createdAt = safeText(stmt.get(), 5);
    return true;
}

void Db::deleteCustomVariant(const std::string& hash) const {
    DbHandle h(dbPath_);
    Statement stmt(h.db, "DELETE FROM custom_variants WHERE hash = ?;");
    checkSqlite(sqlite3_bind_text(stmt.get(), 1, hash.c_str(), -1, SQLITE_TRANSIENT), h.db);
    checkSqlite(sqlite3_step(stmt.get()), h.db);
}

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowT = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&nowT, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

long long nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string makeId() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned long long> dist;
    std::ostringstream ss;
    ss << std::hex << dist(rng) << dist(rng);
    return ss.str();
}

} // namespace fsd
