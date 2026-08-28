"""Opt-in E2E for marketplace facet browsing against the Compose API."""

from __future__ import annotations

import os

import httpx
import pytest


pytestmark = pytest.mark.skipif(
    not os.getenv("E2E_API_URL"),
    reason="set E2E_API_URL to run live Compose E2E checks",
)

_FACETS = {"creator", "variant", "colorMap", "depth", "resolution"}


@pytest.mark.asyncio
async def test_facets_are_public_and_only_report_values_that_filter(e2e_api_url: str) -> None:
    # No session: the browser needs facets before a visitor signs in.
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as guest:
        response = await guest.get("/v1/explore/facets")
        assert response.status_code == 200, response.text
        rows = response.json()["data"]
        assert rows, "expected a seeded catalogue to report at least one facet value"
        assert {row["facet"] for row in rows} <= _FACETS
        assert all(row["count"] > 0 for row in rows)

        # A reported count has to be reproducible by filtering on that value,
        # or the chip would promise results the filter cannot deliver.
        for facet in sorted({row["facet"] for row in rows}):
            row = max((r for r in rows if r["facet"] == facet), key=lambda r: r["count"])
            filtered = await guest.get("/v1/explore", params={facet: row["value"], "limit": 48})
            assert filtered.status_code == 200, filtered.text
            returned = len(filtered.json()["data"])
            assert returned == min(row["count"], 48), f"{facet}={row['value']}: {returned} != {row['count']}"


@pytest.mark.asyncio
async def test_listings_carry_render_metadata(e2e_api_url: str) -> None:
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as guest:
        response = await guest.get("/v1/explore", params={"limit": 12})
        assert response.status_code == 200, response.text
        items = response.json()["data"]
        assert items, "expected a seeded catalogue"
        # Every listing published through the app derives its facets at draft
        # time; only rows predating the backfill may be missing the block.
        described = [item for item in items if item.get("render")]
        assert described, "no listing exposed a render block"
        for item in described:
            render = item["render"]
            assert set(render) <= {
                "variant", "iterations", "width", "height", "colorMap", "colorMode", "viewScale",
            }


@pytest.mark.asyncio
async def test_cursor_is_rejected_when_the_facet_selection_changes(e2e_api_url: str) -> None:
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as guest:
        facets = (await guest.get("/v1/explore/facets")).json()["data"]
        variants = [row for row in facets if row["facet"] == "variant" and row["count"] > 1]
        if len(variants) < 2:
            pytest.skip("needs two variants with more than one listing each")
        first, second = variants[0]["value"], variants[1]["value"]

        page = await guest.get("/v1/explore", params={"variant": first, "limit": 1})
        cursor = page.json()["page"]["nextCursor"]
        assert cursor, "expected a further page"

        same = await guest.get("/v1/explore", params={"variant": first, "limit": 1, "cursor": cursor})
        assert same.status_code == 200, same.text

        # Reusing the cursor under a different facet would silently page through
        # the wrong result set.
        crossed = await guest.get("/v1/explore", params={"variant": second, "limit": 1, "cursor": cursor})
        assert crossed.status_code == 422, crossed.text
