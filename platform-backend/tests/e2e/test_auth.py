"""Live API checks. Run with E2E_API_URL=http://localhost:18000 pytest tests/e2e/test_auth.py."""

from __future__ import annotations

import uuid

import httpx

def test_opaque_session_profile_idempotency_and_logout(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    email = f"auth-{suffix}@example.test"
    password = "correct-horse-01"
    handle = f"creator_{suffix[:8]}"

    with httpx.Client(base_url=e2e_api_url, timeout=10, trust_env=False) as client:
        registered = client.post("/v1/auth/register", json={"email": email, "password": password})
        assert registered.status_code == 201
        assert "fs_session=" in registered.headers["set-cookie"]
        assert password not in registered.text

        profile = client.patch(
            "/v1/me/creator-profile",
            headers={"Idempotency-Key": "profile-0001"},
            json={"handle": handle, "displayName": "Creator"},
        )
        assert profile.status_code == 200
        assert profile.json()["data"]["roles"] == ["creator"]

        replay = client.patch(
            "/v1/me/creator-profile",
            headers={"Idempotency-Key": "profile-0001"},
            json={"handle": handle, "displayName": "Creator"},
        )
        assert replay.status_code == 200
        assert replay.json() == profile.json()
        assert replay.headers["cache-control"] == profile.headers["cache-control"] == "no-store"

        conflict = client.patch(
            "/v1/me/creator-profile",
            headers={"Idempotency-Key": "profile-0001"},
            json={"handle": f"other_{suffix[:8]}", "displayName": "Other"},
        )
        assert conflict.status_code == 409

        csrf_rejected = client.patch(
            "/v1/me/creator-profile",
            headers={"Origin": "http://localhost:3000", "Idempotency-Key": "profile-cross-site"},
            json={"handle": handle, "displayName": "Creator"},
        )
        assert csrf_rejected.status_code == 403

        logged_out = client.post("/v1/auth/logout")
        assert logged_out.status_code == 204
        assert client.get("/v1/me").status_code == 401
