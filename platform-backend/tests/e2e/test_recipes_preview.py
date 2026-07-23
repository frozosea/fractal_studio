"""Live Compose checks for M2 recipe persistence and bounded preview contract."""

from __future__ import annotations

import os
import uuid

import httpx
import pytest


def _optional_database_url() -> str | None:
    """Live DB assertions are enabled only when test runner may access development PostgreSQL."""
    return os.getenv("E2E_DATABASE_URL")


def test_recipes_are_immutable_deduplicated_and_cursor_listed(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    request_id = str(uuid.uuid4())
    with httpx.Client(base_url=e2e_api_url, timeout=10, trust_env=False) as client:
        registered = client.post(
            "/v1/auth/register",
            json={"email": f"studio-{suffix}@example.test", "password": "correct-horse-01"},
        )
        assert registered.status_code == 201

        created = client.post(
            "/v1/recipes",
            headers={"Idempotency-Key": "recipe-0001", "X-Request-ID": request_id},
            json={"canonicalSpec": {"version": 1, "seed": 42}},
        )
        assert created.status_code == 201
        recipe_id = created.json()["data"]["id"]

        same_recipe = client.post(
            "/v1/recipes",
            headers={"Idempotency-Key": "recipe-0002"},
            json={"canonicalSpec": {"seed": 42, "version": 1, "scale": 4.0}},
        )
        assert same_recipe.status_code == 200
        assert same_recipe.json()["data"]["id"] == recipe_id

        listed = client.get("/v1/me/recipes")
        assert listed.status_code == 200
        assert listed.json()["data"][0]["id"] == recipe_id
        assert "page" in listed.json()

    if database_url := _optional_database_url():
        import psycopg

        with psycopg.connect(database_url) as connection:
            audit = connection.execute(
                """
                SELECT count(*) FROM audit_events
                WHERE subject_type = 'fractal_recipe' AND subject_id = %s
                  AND action = 'recipe.created' AND metadata_json ->> 'requestId' = %s
                """,
                (recipe_id, request_id),
            ).fetchone()
        assert audit == (1,)


@pytest.mark.skipif(
    os.getenv("E2E_COMPUTE_AVAILABLE") != "1",
    reason="set E2E_COMPUTE_AVAILABLE=1 when private Compute contract stub/server is running",
)
def test_preview_returns_png_and_enforces_bounds(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    with httpx.Client(base_url=e2e_api_url, timeout=10, trust_env=False) as client:
        registered = client.post(
            "/v1/auth/register",
            json={"email": f"preview-{suffix}@example.test", "password": "correct-horse-01"},
        )
        assert registered.status_code == 201
        preview = client.post(
            "/v1/studio/preview",
            json={"canonicalSpec": {"version": 1, "seed": 42}, "width": 16, "height": 16},
        )
        assert preview.status_code == 200
        assert preview.headers["content-type"].startswith("image/png")
        assert preview.content.startswith(b"\x89PNG\r\n\x1a\n")

        oversized = client.post(
            "/v1/studio/preview",
            json={"canonicalSpec": {"version": 1}, "width": 4096, "height": 4096},
        )
        assert oversized.status_code == 422

        for _ in range(29):
            limited = client.post(
                "/v1/studio/preview",
                json={"canonicalSpec": {"version": 1}, "width": 1, "height": 1},
            )
            assert limited.status_code == 200
        assert (
            client.post(
                "/v1/studio/preview",
                json={"canonicalSpec": {"version": 1}, "width": 1, "height": 1},
            ).status_code
            == 429
        )

        listed = client.get("/v1/me/recipes")
        assert listed.status_code == 200
        assert listed.json()["data"] == []

    if database_url := _optional_database_url():
        import psycopg

        with psycopg.connect(database_url) as connection:
            durable_count = connection.execute(
                """
                SELECT (SELECT count(*) FROM render_jobs WHERE owner_id = %s)
                     + (SELECT count(*) FROM assets WHERE owner_id = %s)
                     + (SELECT count(*) FROM outbox_events WHERE aggregate_id = %s::uuid)
                """,
                (registered.json()["data"]["id"],) * 3,
            ).fetchone()
        assert durable_count == (0,)
