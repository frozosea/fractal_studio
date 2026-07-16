#pragma once

#include <complex>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fsd::compute {

enum class SpecialPointKind {
    HyperbolicCenter,
    Misiurewicz,
};

struct SpecialPointViewport {
    bool enabled = false;
    double center_re = -0.75;
    double center_im = 0.0;
    double scale = 3.0;
    int width = 1200;
    int height = 800;
    // Optional decimal strings for deep zoom; when non-empty they carry the
    // full-precision center and override center_re/center_im in the
    // high-precision solver (same convention as the perturbation renderer).
    std::string center_re_str;
    std::string center_im_str;
};

struct SpecialPointEnumRequest {
    SpecialPointKind kind = SpecialPointKind::HyperbolicCenter;

    int period_min = 1;
    int period_max = 8;

    int preperiod_min = 1;
    int preperiod_max = 4;
    int misiurewicz_period_min = 1;
    int misiurewicz_period_max = 4;

    int max_newton_iter = 60;
    int max_seed_batches = 80;
    int seeds_per_batch = 2048;

    double newton_eps = 1e-13;
    double classify_eps = 1e-10;
    double root_merge_eps = 1e-9;

    bool include_variant_existence = true;
    bool include_rejected_debug = false;
    bool visible_only = false;
    SpecialPointViewport viewport;
};

struct OrbitClassification {
    bool found_repeat = false;
    bool is_center = false;
    bool is_misiurewicz = false;
    int preperiod = -1;
    int period = -1;
    double repeat_error = 0.0;
    std::vector<std::complex<double>> orbit;
};

struct VariantExistence {
    std::string variant_name;
    bool exists = false;
    bool same_orbit_as_mandelbrot = false;
    int actual_preperiod = -1;
    int actual_period = -1;
    double repeat_error = 0.0;
    std::string reason;
};

struct SpecialPointResult {
    std::string id;
    SpecialPointKind kind = SpecialPointKind::HyperbolicCenter;
    int preperiod = 0;
    int period = 1;
    double re = 0.0;
    double im = 0.0;
    // Full-precision decimal coordinates, set by the high-precision solver
    // (empty on the double path). re/im hold the rounded values.
    std::string re_str;
    std::string im_str;
    int prec_bits = 0;  // MPFR precision used; 0 = double path
    bool converged = false;
    bool accepted = false;
    bool fallback = false;
    bool visible = true;
    double residual = 0.0;
    int newton_iterations = 0;
    OrbitClassification actual;
    std::vector<VariantExistence> variants;
    std::string reason;
};

struct SpecialPointEnumResponse {
    bool complete = false;
    std::string status = "idle";
    int accepted_count = 0;
    int expected_count = 0;
    int seed_count = 0;
    int newton_success_count = 0;
    int rejected_count = 0;
    std::vector<SpecialPointResult> points;
    std::vector<SpecialPointResult> rejected_debug;
    std::string warning;
};

struct SpecialPointSearchRequest {
    SpecialPointKind kind = SpecialPointKind::HyperbolicCenter;
    int period_min = 1;
    int period_max = 8;
    int preperiod_min = 1;
    int preperiod_max = 4;
    int seed_budget = 2000;
    int max_newton_iter = 60;
    double newton_eps = 1e-13;
    double classify_eps = 1e-10;
    double root_merge_eps = 1e-10;
    bool visible_only = true;
    bool include_variant_compatibility = true;
    SpecialPointViewport viewport;
};

struct SpecialPointSearchResponse {
    std::string status = "idle";
    bool sampled = false;
    int accepted_count = 0;
    int seed_count = 0;
    int newton_success_count = 0;
    int rejected_count = 0;
    int fallback_count = 0;
    std::vector<SpecialPointResult> points;
    std::string warning;
};

using SpecialPointProgressCallback = std::function<bool(
    int task_index,
    int task_count,
    int accepted_count,
    int expected_count,
    int seed_count,
    int batch_index)>;

using SpecialPointSearchProgressCallback = std::function<bool(
    int period,
    int period_index,
    int period_count,
    int accepted_count,
    int seed_count)>;

// Kept separate from progress reporting so tight Newton/orbit loops can poll
// cancellation without writing progress.json on every check. Search workers
// may invoke it concurrently, so callbacks must be thread-safe.
using SpecialPointCancelCallback = std::function<bool()>;

std::pair<std::complex<double>, std::complex<double>>
eval_center_f_df(std::complex<double> c, int period);

std::pair<std::complex<double>, std::complex<double>>
eval_misiurewicz_f_df(std::complex<double> c, int preperiod, int period);

OrbitClassification classify_critical_orbit_mandelbrot(
    std::complex<double> c,
    int max_iter,
    double eps,
    const SpecialPointCancelCallback& should_cancel = {});

OrbitClassification classify_critical_orbit(
    std::complex<double> c,
    int max_iter,
    double eps,
    const SpecialPointCancelCallback& should_cancel = {});

std::vector<VariantExistence> classify_variant_existence(
    std::complex<double> c,
    SpecialPointKind requested_kind,
    int requested_preperiod,
    int requested_period,
    double eps,
    const SpecialPointCancelCallback& should_cancel = {});

SpecialPointResult newton_solve_center(
    std::complex<double> initial,
    int period,
    const SpecialPointEnumRequest& req,
    const SpecialPointCancelCallback& should_cancel = {});

SpecialPointResult find_hyperbolic_center_near(
    std::complex<double> initial,
    int period,
    const SpecialPointSearchRequest& req);

SpecialPointResult newton_solve_misiurewicz(
    std::complex<double> initial,
    int preperiod,
    int period,
    const SpecialPointEnumRequest& req,
    const SpecialPointCancelCallback& should_cancel = {});

int expected_center_count(int period);
int expected_misiurewicz_count(int preperiod, int period);

SpecialPointEnumResponse enumerate_special_points(
    const SpecialPointEnumRequest& req,
    const SpecialPointProgressCallback& progress = {},
    const SpecialPointCancelCallback& should_cancel = {});

SpecialPointSearchResponse search_special_points(
    const SpecialPointSearchRequest& req,
    const SpecialPointSearchProgressCallback& progress = {},
    const SpecialPointCancelCallback& should_cancel = {});

// High-precision (MPFR) deep-zoom path, implemented in special_points_hp.cpp.
// search_special_points() delegates to it when the viewport scale drops below
// special_points_hp_scale_threshold(); it can also be called directly.
bool special_points_hp_available();
double special_points_hp_scale_threshold();
bool special_points_search_wants_hp(const SpecialPointSearchRequest& req);
SpecialPointSearchResponse search_special_points_hp(
    const SpecialPointSearchRequest& req,
    const SpecialPointSearchProgressCallback& progress = {},
    const SpecialPointCancelCallback& should_cancel = {});

bool point_in_viewport(const SpecialPointViewport& viewport, std::complex<double> c);
std::string special_point_kind_name(SpecialPointKind kind);

} // namespace fsd::compute
