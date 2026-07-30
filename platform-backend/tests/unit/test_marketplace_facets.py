"""Facet band derivation for marketplace browsing."""

from __future__ import annotations

import pytest

from app.marketplace.repository import (
    DEPTH_BUCKETS,
    DEPTH_KEYS,
    RESOLUTION_BUCKETS,
    RESOLUTION_KEYS,
    _band_case,
    _band_predicates,
)


def test_bands_cover_every_key() -> None:
    assert set(DEPTH_BUCKETS) == set(DEPTH_KEYS)
    assert set(RESOLUTION_BUCKETS) == set(RESOLUTION_KEYS)


def test_first_band_is_open_at_the_bottom_and_last_at_the_top() -> None:
    # Nothing may fall between bands, so the lowest has no lower bound and the
    # highest no upper bound.
    assert DEPTH_BUCKETS["le512"] == "l.iterations <= 512"
    assert DEPTH_BUCKETS["gt2048"] == "l.iterations > 2048"
    assert RESOLUTION_BUCKETS["le1mp"] == "l.output_width * l.output_height <= 1000000"
    assert RESOLUTION_BUCKETS["gt8mp"] == "l.output_width * l.output_height > 8000000"


def test_middle_bands_are_half_open_so_boundaries_land_in_exactly_one() -> None:
    assert DEPTH_BUCKETS["le1024"] == "l.iterations > 512 AND l.iterations <= 1024"
    assert DEPTH_BUCKETS["le2048"] == "l.iterations > 1024 AND l.iterations <= 2048"


@pytest.mark.parametrize(
    ("value", "expected"),
    [(1, "le512"), (512, "le512"), (513, "le1024"), (1024, "le1024"), (2048, "le2048"), (2049, "gt2048")],
)
def test_every_depth_lands_in_exactly_one_band(value: int, expected: str) -> None:
    matched = [key for key, predicate in DEPTH_BUCKETS.items() if _evaluate(predicate, value)]
    assert matched == [expected]


def test_case_arms_follow_the_same_boundaries_as_the_predicates() -> None:
    # The counting CASE and the filtering predicates are generated from one set
    # of bounds; if they drifted, a facet chip would report a count that its own
    # filter could not reproduce.
    case = _band_case("x", (("low", 10), ("high", None)))
    assert case == "CASE WHEN x <= 10 THEN 'low' ELSE 'high' END"
    assert _band_predicates("x", (("low", 10), ("high", None))) == {
        "low": "x <= 10",
        "high": "x > 10",
    }


def _evaluate(predicate: str, value: int) -> bool:
    """Evaluate a generated SQL band predicate in Python for a single value."""
    expression = predicate.replace("l.iterations", str(value)).replace(" AND ", " and ")
    return bool(eval(expression))  # noqa: S307 - fixed, module-generated fragments
