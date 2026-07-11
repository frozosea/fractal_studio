#include "compute/special_points.hpp"

#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(FSD_HAS_MPFR)
#  include <mpfr.h>
#endif

namespace {

using fsd::compute::SpecialPointEnumRequest;
using fsd::compute::SpecialPointKind;
using fsd::compute::SpecialPointSearchRequest;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

SpecialPointEnumRequest base_request() {
    SpecialPointEnumRequest req;
    req.max_newton_iter = 80;
    req.max_seed_batches = 120;
    req.seeds_per_batch = 4096;
    req.newton_eps = 1e-13;
    req.classify_eps = 1e-10;
    req.root_merge_eps = 1e-9;
    req.include_variant_existence = true;
    return req;
}

void expect_center_period(int period, int expected) {
    SpecialPointEnumRequest req = base_request();
    req.kind = SpecialPointKind::HyperbolicCenter;
    req.period_min = period;
    req.period_max = period;
    const auto resp = fsd::compute::enumerate_special_points(req);
    require(resp.complete, "center enumeration did not complete for period " + std::to_string(period));
    require(resp.expected_count == expected, "unexpected center expected count for period " + std::to_string(period));
    require(resp.accepted_count == expected, "unexpected center accepted count for period " + std::to_string(period));
    for (const auto& p : resp.points) {
        require(p.accepted, "center point was not accepted");
        require(p.actual.is_center, "center point did not classify as center");
        require(p.actual.period == period, "center point classified with wrong period");
        require(!p.variants.empty(), "center point missing variant existence results");
        require(p.variants.front().variant_name == "Mandelbrot" && p.variants.front().exists,
                "Mandelbrot variant did not accept Mandelbrot point");
    }
}

void expect_misiurewicz_2_1() {
    SpecialPointEnumRequest req = base_request();
    req.kind = SpecialPointKind::Misiurewicz;
    req.preperiod_min = 2;
    req.preperiod_max = 2;
    req.misiurewicz_period_min = 1;
    req.misiurewicz_period_max = 1;
    const auto resp = fsd::compute::enumerate_special_points(req);
    require(resp.complete, "misiurewicz m=2 p=1 enumeration did not complete");
    require(resp.expected_count == 1, "unexpected misiurewicz m=2 p=1 expected count");
    require(resp.accepted_count == 1, "unexpected misiurewicz m=2 p=1 accepted count");
    const auto& p = resp.points.front();
    require(p.actual.is_misiurewicz, "misiurewicz point did not classify as Misiurewicz");
    require(p.actual.preperiod == 2 && p.actual.period == 1, "wrong misiurewicz classification");
    require(std::abs(p.re + 2.0) < 1e-8 && std::abs(p.im) < 1e-8,
            "misiurewicz m=2 p=1 root is not c=-2: " + std::to_string(p.re) + ", " + std::to_string(p.im));
}

void expect_high_period_local_center() {
    SpecialPointEnumRequest req = base_request();
    req.kind = SpecialPointKind::HyperbolicCenter;
    const auto p = fsd::compute::newton_solve_center(
        {-0.7601060136, 0.0803662122},
        207,
        req);
    require(p.converged, "high-period local center did not converge");
    require(p.accepted, "high-period local center was not accepted");
    require(p.actual.is_center && p.actual.period == 207,
            "high-period local center classified with wrong period");
    require(p.residual < req.classify_eps, "high-period local center residual exceeds classifier tolerance");

    SpecialPointSearchRequest search;
    search.kind = SpecialPointKind::HyperbolicCenter;
    search.period_min = 1;
    search.period_max = 8192;
    search.seed_budget = 2000;
    search.max_newton_iter = 80;
    search.newton_eps = 1e-13;
    search.classify_eps = 1e-10;
    search.root_merge_eps = 1e-10;
    search.visible_only = true;
    search.include_variant_compatibility = true;
    search.viewport.enabled = true;
    search.viewport.center_re = -0.7601060136;
    search.viewport.center_im = 0.0803662122;
    search.viewport.scale = 2.171e-8;
    search.viewport.width = 1200;
    search.viewport.height = 800;
    const auto resp = fsd::compute::search_special_points(search);
    require(resp.status == "completed", "high-period local center search did not complete");
    require(resp.accepted_count >= 1, "high-period local center search did not find any center");
    require(resp.seed_count >= 207, "high-period local center search skipped lower-period equations");
    bool found_period_207 = false;
    for (const auto& point : resp.points) {
        if (point.actual.is_center && point.actual.period == 207) found_period_207 = true;
    }
    require(found_period_207, "high-period local center search did not mark the expected period");
}

// Below the HP threshold this routes to the MPFR ball-period solver; the
// assertions are engine-agnostic so a non-MPFR build still passes via the
// double path.
void expect_deep_zoom_local_center_search() {
    SpecialPointSearchRequest search;
    search.kind = SpecialPointKind::HyperbolicCenter;
    search.period_min = 1;
    search.period_max = 8192;
    search.seed_budget = 2000;
    search.max_newton_iter = 60;
    search.newton_eps = 1e-13;
    search.classify_eps = 1e-10;
    search.root_merge_eps = 1e-10;
    search.visible_only = true;
    search.include_variant_compatibility = true;
    search.viewport.enabled = true;
    search.viewport.center_re = -0.7520160599;
    search.viewport.center_im = 0.0361977261;
    search.viewport.scale = 9.549e-10;
    search.viewport.width = 1200;
    search.viewport.height = 800;

    const auto resp = fsd::compute::search_special_points(search);
    require(resp.status == "completed", "deep-zoom local center search did not complete");
    require(resp.accepted_count >= 1, "deep-zoom local center search did not find any center");
    // The viewport contains the primitive period-954 nucleus and its period-
    // tripled satellite (2862). Double precision cannot verify the primitive
    // nucleus (its Newton noise floor sits above the acceptance threshold) and
    // lands on the satellite; the MPFR path finds the primitive one, matching
    // the period-ascending search contract.
    const int expected_period = fsd::compute::special_points_hp_available() ? 954 : 2862;
    bool found_expected = false;
    for (const auto& point : resp.points) {
        require(point.visible, "deep-zoom local center search returned a non-visible point");
        if (point.actual.is_center && point.actual.period == expected_period) found_expected = true;
    }
    require(found_expected, "deep-zoom local center search did not mark the expected period");
    if (fsd::compute::special_points_hp_available()) {
        require(resp.points.front().prec_bits > 0,
                "deep-zoom search did not use the high-precision solver");
        require(!resp.points.front().re_str.empty() && !resp.points.front().im_str.empty(),
                "high-precision result missing string coordinates");
        require(resp.points.front().residual < 1e-30,
                "high-precision nucleus residual unexpectedly large");
    }
}

void expect_overview_period_ordered_center_search() {
    SpecialPointSearchRequest search;
    search.kind = SpecialPointKind::HyperbolicCenter;
    search.period_min = 1;
    search.period_max = 8192;
    search.seed_budget = 160;
    search.max_newton_iter = 60;
    search.newton_eps = 1e-13;
    search.classify_eps = 1e-10;
    search.root_merge_eps = 1e-10;
    search.visible_only = true;
    search.include_variant_compatibility = false;
    search.viewport.enabled = true;
    search.viewport.center_re = -0.75;
    search.viewport.center_im = 0.0;
    search.viewport.scale = 3.0;
    search.viewport.width = 1920;
    search.viewport.height = 1080;

    const auto resp = fsd::compute::search_special_points(search);
    require(resp.status == "completed", "overview period-ordered center search did not complete");
    require(resp.accepted_count >= 1, "overview period-ordered center search did not find any center");
    require(resp.seed_count <= 128, "overview period-ordered center search should stop after the first wave");
    bool found_period_1 = false;
    for (const auto& p : resp.points) {
        require(p.visible, "overview period-ordered center search returned a non-visible point");
        if (p.actual.is_center && p.actual.period == 1 &&
            std::abs(p.re) < 1e-12 && std::abs(p.im) < 1e-12) {
            found_period_1 = true;
        }
    }
    require(found_period_1, "overview period-ordered center search did not mark c=0");
}

void expect_viewport_sampled_center_search() {
    SpecialPointSearchRequest search;
    search.kind = SpecialPointKind::HyperbolicCenter;
    search.period_min = 1;
    search.period_max = 8192;
    search.seed_budget = 160;
    search.max_newton_iter = 60;
    search.newton_eps = 1e-13;
    search.classify_eps = 1e-10;
    search.root_merge_eps = 1e-10;
    search.visible_only = true;
    search.include_variant_compatibility = false;
    search.viewport.enabled = true;
    search.viewport.center_re = -0.7511886968;
    search.viewport.center_im = 0.0278611298;
    search.viewport.scale = 2.002e-10;
    search.viewport.width = 1920;
    search.viewport.height = 1080;

    const auto resp = fsd::compute::search_special_points(search);
    require(resp.status == "completed", "viewport-sampled center search did not complete");
    if (!fsd::compute::special_points_hp_available()) {
        require(resp.sampled, "viewport-sampled center search did not sample the viewport");
    }
    require(resp.accepted_count >= 1, "viewport-sampled center search did not find any center");
    // Primitive period-1381 nucleus vs. its period-doubled satellite (2762);
    // same double-precision blindness as in the 954/2862 viewport above.
    const int expected_period = fsd::compute::special_points_hp_available() ? 1381 : 2762;
    bool found_expected = false;
    for (const auto& point : resp.points) {
        require(point.visible, "viewport-sampled center search returned a non-visible point");
        if (point.actual.is_center && point.actual.period == expected_period) found_expected = true;
    }
    require(found_expected, "viewport-sampled center search did not mark the expected period");
}

#if defined(FSD_HAS_MPFR)

// |a - b| for two decimal strings, evaluated in MPFR.
double decimal_string_diff(const std::string& a, const std::string& b) {
    mpfr_t va, vb;
    mpfr_inits2(320, va, vb, static_cast<mpfr_ptr>(nullptr));
    mpfr_set_str(va, a.c_str(), 10, MPFR_RNDN);
    mpfr_set_str(vb, b.c_str(), 10, MPFR_RNDN);
    mpfr_sub(va, va, vb, MPFR_RNDN);
    mpfr_abs(va, va, MPFR_RNDN);
    const double out = mpfr_get_d(va, MPFR_RNDN);
    mpfr_clears(va, vb, static_cast<mpfr_ptr>(nullptr));
    return out;
}

SpecialPointSearchRequest hp_center_request() {
    SpecialPointSearchRequest search;
    search.kind = SpecialPointKind::HyperbolicCenter;
    search.period_min = 1;
    search.period_max = 8192;
    search.visible_only = true;
    search.include_variant_compatibility = true;
    search.viewport.enabled = true;
    search.viewport.width = 1200;
    search.viewport.height = 800;
    return search;
}

// The HP solver must agree with the double path where both work: the
// period-207 nucleus near (-0.7601060136, 0.0803662122).
fsd::compute::SpecialPointResult expect_hp_matches_double_at_moderate_zoom() {
    SpecialPointEnumRequest req = base_request();
    const auto reference = fsd::compute::newton_solve_center(
        {-0.7601060136, 0.0803662122}, 207, req);
    require(reference.accepted, "double-path period-207 reference did not converge");

    SpecialPointSearchRequest search = hp_center_request();
    search.viewport.center_re = -0.7601060136;
    search.viewport.center_im = 0.0803662122;
    search.viewport.scale = 2.171e-8;

    const auto resp = fsd::compute::search_special_points_hp(search);
    require(resp.status == "completed", "hp moderate-zoom center search did not complete");
    require(resp.accepted_count >= 1, "hp moderate-zoom center search found no center");
    const auto& p = resp.points.front();
    require(p.accepted && p.actual.is_center && p.actual.period == 207,
            "hp moderate-zoom center search returned wrong classification");
    require(std::abs(p.re - reference.re) < 1e-10 && std::abs(p.im - reference.im) < 1e-10,
            "hp nucleus disagrees with the double-path nucleus");
    require(!p.re_str.empty() && !p.im_str.empty() && p.prec_bits >= 128,
            "hp result missing high-precision coordinates");
    return p;
}

// Re-searching a 1e-24 viewport centered on the found nucleus strings must
// find the same period-207 nucleus again — double precision cannot even
// represent this viewport, so this exercises the string path end to end.
void expect_hp_deep_zoom_self_consistency(const fsd::compute::SpecialPointResult& moderate) {
    SpecialPointSearchRequest search = hp_center_request();
    search.viewport.center_re = moderate.re;
    search.viewport.center_im = moderate.im;
    search.viewport.center_re_str = moderate.re_str;
    search.viewport.center_im_str = moderate.im_str;
    search.viewport.scale = 1e-24;

    const auto resp = fsd::compute::search_special_points(search);
    require(resp.status == "completed", "hp deep self-consistency search did not complete");
    require(resp.accepted_count >= 1, "hp deep self-consistency search found no center");
    const auto& p = resp.points.front();
    require(p.accepted && p.visible, "hp deep self-consistency point not accepted/visible");
    require(p.actual.is_center && p.actual.period == 207,
            "hp deep self-consistency returned wrong period");
    require(p.prec_bits > moderate.prec_bits,
            "hp deep re-solve did not raise precision with depth");
    require(decimal_string_diff(p.re_str, moderate.re_str) < 1e-30,
            "hp deep re-solve moved the nucleus (re)");
    require(decimal_string_diff(p.im_str, moderate.im_str) < 1e-30,
            "hp deep re-solve moved the nucleus (im)");
}

// String round-trip for a Misiurewicz solve: a 1e-14 viewport just off c=-2
// must snap onto the exact m=2, p=1 point at -2.
void expect_hp_misiurewicz_string_roundtrip() {
    SpecialPointSearchRequest search;
    search.kind = SpecialPointKind::Misiurewicz;
    search.preperiod_min = 2;
    search.preperiod_max = 2;
    search.period_min = 1;
    search.period_max = 1;
    search.visible_only = true;
    search.include_variant_compatibility = true;
    search.viewport.enabled = true;
    search.viewport.center_re = -2.0;
    search.viewport.center_im = 0.0;
    search.viewport.center_re_str = "-2.0000000000000000000000001";
    search.viewport.center_im_str = "0";
    search.viewport.scale = 1e-14;
    search.viewport.width = 1200;
    search.viewport.height = 800;

    const auto resp = fsd::compute::search_special_points(search);
    require(resp.status == "completed", "hp Misiurewicz string search did not complete");
    require(resp.accepted_count == 1, "hp Misiurewicz string search did not accept exactly one point");
    const auto& p = resp.points.front();
    require(p.actual.is_misiurewicz && p.actual.preperiod == 2 && p.actual.period == 1,
            "hp Misiurewicz string search classified wrong (m, p)");
    require(!p.re_str.empty() && decimal_string_diff(p.re_str, "-2") < 1e-20,
            "hp Misiurewicz root re_str is not -2");
    require(decimal_string_diff(p.im_str, "0") < 1e-20,
            "hp Misiurewicz root im_str is not 0");
}

void run_hp_tests() {
    const auto moderate = expect_hp_matches_double_at_moderate_zoom();
    expect_hp_deep_zoom_self_consistency(moderate);
    expect_hp_misiurewicz_string_roundtrip();
}

#else

void run_hp_tests() {
    std::cout << "special_points_smoke: MPFR unavailable, skipping high-precision tests\n";
}

#endif // FSD_HAS_MPFR

SpecialPointSearchRequest base_search_request() {
    SpecialPointSearchRequest req;
    req.period_min = 1;
    req.period_max = 4;
    req.seed_budget = 1200;
    req.max_newton_iter = 80;
    req.newton_eps = 1e-13;
    req.classify_eps = 1e-10;
    req.root_merge_eps = 1e-10;
    req.include_variant_compatibility = true;
    req.visible_only = true;
    req.viewport.enabled = true;
    req.viewport.center_re = -1.0;
    req.viewport.center_im = 0.0;
    req.viewport.scale = 0.8;
    req.viewport.width = 1200;
    req.viewport.height = 800;
    return req;
}

void expect_local_center_search() {
    SpecialPointSearchRequest req = base_search_request();
    const auto near_period_2 = fsd::compute::find_hyperbolic_center_near({-0.85, 0.05}, 2, req);
    require(near_period_2.converged, "local Newton period-2 center did not converge");
    require(near_period_2.accepted, "local Newton period-2 center was not accepted");
    require(near_period_2.actual.is_center && near_period_2.actual.period == 2, "local Newton classified wrong period");
    require(std::abs(near_period_2.re + 1.0) < 1e-8 && std::abs(near_period_2.im) < 1e-8,
            "local Newton did not converge to c=-1");
    require(!near_period_2.variants.empty(), "local Newton missing variant compatibility");
    require(near_period_2.variants.front().variant_name == "Mandelbrot" && near_period_2.variants.front().exists,
            "Mandelbrot compatibility missing for local center");

    const auto degenerate = fsd::compute::find_hyperbolic_center_near({0.0, 0.0}, 2, req);
    require(degenerate.converged, "degenerate local Newton did not converge");
    require(!degenerate.accepted, "period-1 center accepted as period 2 in local Newton");
    require(degenerate.actual.period == 1, "degenerate local Newton actual period not detected");

    const auto resp = fsd::compute::search_special_points(req);
    require(resp.status == "completed", "viewport center search did not complete");
    require(resp.accepted_count >= 1, "viewport center search found no centers");
    bool found_period_2 = false;
    for (const auto& p : resp.points) {
        require(p.visible, "viewport search returned non-visible point");
        require(p.accepted && p.actual.is_center, "viewport search returned non-center point");
        if (p.period == 2 && std::abs(p.re + 1.0) < 1e-8 && std::abs(p.im) < 1e-8) {
            found_period_2 = true;
        }
    }
    require(found_period_2, "viewport search did not find the period-2 center");

    SpecialPointSearchRequest fallback_req = req;
    fallback_req.period_min = 2;
    fallback_req.period_max = 2;
    fallback_req.viewport.center_re = 0.0;
    fallback_req.viewport.center_im = 0.0;
    fallback_req.viewport.scale = 1.0;
    const auto fallback_resp = fsd::compute::search_special_points(fallback_req);
    require(fallback_resp.status == "completed", "fallback local center search did not complete");
    require(fallback_resp.accepted_count == 0, "fallback local center search should not count exact matches");
    require(fallback_resp.fallback_count == 1, "fallback local center search did not return fallback");
    require(!fallback_resp.points.empty() && fallback_resp.points.front().fallback,
            "fallback local center point not flagged");
    require(fallback_resp.points.front().actual.is_center && fallback_resp.points.front().actual.period == 1,
            "fallback local center did not report the largest classified candidate period");

    SpecialPointSearchRequest misi_req = req;
    misi_req.kind = SpecialPointKind::Misiurewicz;
    misi_req.preperiod_min = 2;
    misi_req.preperiod_max = 2;
    misi_req.period_min = 1;
    misi_req.period_max = 1;
    misi_req.viewport.center_re = -2.0;
    misi_req.viewport.center_im = 0.0;
    misi_req.viewport.scale = 0.2;
    const auto misi_resp = fsd::compute::search_special_points(misi_req);
    require(misi_resp.status == "completed", "viewport Misiurewicz search did not complete");
    require(misi_resp.accepted_count == 1, "viewport Misiurewicz search should stop after one point");
    require(misi_resp.points.front().actual.is_misiurewicz, "viewport Misiurewicz point did not classify as Misiurewicz");
    require(misi_resp.points.front().actual.preperiod == 2 && misi_resp.points.front().actual.period == 1,
            "viewport Misiurewicz point classified with wrong preperiod/period");
}

} // namespace

int main() {
    try {
        require(fsd::compute::expected_center_count(1) == 1, "center count p=1");
        require(fsd::compute::expected_center_count(2) == 1, "center count p=2");
        require(fsd::compute::expected_center_count(3) == 3, "center count p=3");
        require(fsd::compute::expected_center_count(4) == 6, "center count p=4");

        require(fsd::compute::expected_misiurewicz_count(1, 1) == 0, "misiurewicz count m=1 p=1");
        require(fsd::compute::expected_misiurewicz_count(2, 1) == 1, "misiurewicz count m=2 p=1");
        require(fsd::compute::expected_misiurewicz_count(2, 2) == 2, "misiurewicz count m=2 p=2");
        require(fsd::compute::expected_misiurewicz_count(3, 2) == 3, "misiurewicz count m=3 p=2");
        require(fsd::compute::expected_misiurewicz_count(4, 3) == 21, "misiurewicz count m=4 p=3");

        SpecialPointEnumRequest req = base_request();
        const auto deg_center = fsd::compute::newton_solve_center({0.0, 0.0}, 2, req);
        require(deg_center.converged, "degenerate center did not converge");
        require(!deg_center.accepted, "period-1 center accepted as period 2");
        require(deg_center.actual.period == 1, "degenerate center actual period not detected");

        req.kind = SpecialPointKind::Misiurewicz;
        const auto deg_misi = fsd::compute::newton_solve_misiurewicz({0.0, 0.0}, 2, 1, req);
        require(deg_misi.converged, "degenerate Misiurewicz seed did not converge");
        require(!deg_misi.accepted, "center accepted as Misiurewicz");
        require(deg_misi.actual.is_center, "degenerate Misiurewicz seed did not classify as center");

        expect_center_period(1, 1);
        expect_center_period(2, 1);
        expect_center_period(3, 3);
        expect_center_period(4, 6);
        expect_misiurewicz_2_1();
        expect_local_center_search();
        expect_overview_period_ordered_center_search();
        expect_high_period_local_center();
        expect_deep_zoom_local_center_search();
        expect_viewport_sampled_center_search();
        run_hp_tests();
    } catch (const std::exception& e) {
        std::cerr << "special_points_smoke failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "special_points_smoke passed\n";
    return 0;
}
