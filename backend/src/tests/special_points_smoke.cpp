#include "compute/special_points.hpp"

#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <string>

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
}

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
        expect_high_period_local_center();
    } catch (const std::exception& e) {
        std::cerr << "special_points_smoke failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "special_points_smoke passed\n";
    return 0;
}
