"""Opt-in E2E for the public creator profile endpoints."""

from __future__ import annotations

import os

import httpx
import pytest


pytestmark = pytest.mark.skipif(
    not os.getenv("E2E_API_URL"),
    reason="set E2E_API_URL to run live Compose E2E checks",
)


async def _any_creator(client: httpx.AsyncClient) -> str:
    listings = (await client.get("/v1/explore", params={"limit": 1})).json()["data"]
    if not listings:
        pytest.skip("needs a published catalogue")
    return str(listings[0]["creator"]["handle"])


@pytest.mark.asyncio
async def test_profile_and_listings_are_readable_without_a_session(e2e_api_url: str) -> None:
    # The page is meant to be shared, so it has to work for a signed-out visitor.
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as guest:
        handle = await _any_creator(guest)

        profile = await guest.get(f"/v1/creators/{handle}")
        assert profile.status_code == 200, profile.text
        data = profile.json()["data"]
        assert data["handle"] == handle
        assert data["displayName"]
        assert data["publishedCount"] >= 1

        listings = await guest.get(f"/v1/creators/{handle}/listings", params={"limit": 48})
        assert listings.status_code == 200, listings.text
        items = listings.json()["data"]
        assert items, "a creator reported as published should have listings"
        # Exact handle match, not the catalogue's substring `creator` filter:
        # a profile page must never mix in a creator with a longer handle.
        assert {item["creator"]["handle"] for item in items} == {handle}
        assert len(items) == min(data["publishedCount"], 48)


@pytest.mark.asyncio
async def test_unknown_and_malformed_handles_are_rejected(e2e_api_url: str) -> None:
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as guest:
        assert (await guest.get("/v1/creators/no_such_creator_here")).status_code == 404
        # Uppercase and hyphens are outside the handle grammar.
        assert (await guest.get("/v1/creators/BAD-Handle")).status_code == 422
        assert (await guest.get("/v1/creators/ab")).status_code == 422


@pytest.mark.asyncio
async def test_listings_are_ordered_newest_first_and_page_stably(e2e_api_url: str) -> None:
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as guest:
        handle = await _any_creator(guest)
        first = await guest.get(f"/v1/creators/{handle}/listings", params={"limit": 2})
        body = first.json()
        published = [item["publishedAt"] for item in body["data"]]
        assert published == sorted(published, reverse=True)

        cursor = body["page"]["nextCursor"]
        if not cursor:
            pytest.skip("creator has a single page of work")
        second = await guest.get(f"/v1/creators/{handle}/listings", params={"limit": 2, "cursor": cursor})
        assert second.status_code == 200, second.text
        # A profile should not reshuffle: the second page must not repeat the first.
        assert not ({item["id"] for item in body["data"]} & {item["id"] for item in second.json()["data"]})
