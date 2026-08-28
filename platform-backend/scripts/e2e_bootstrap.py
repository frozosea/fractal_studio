"""Compose-only fixture bootstrap; it writes no business state except fixed operator/disabled roles."""

from __future__ import annotations

import asyncio
import os

import asyncpg
import httpx


async def _register(client: httpx.AsyncClient, *, email: str, password: str) -> str | None:
    """Create the fixed account; return its id, or None when it already exists.

    An existing disabled account legitimately answers login with 401, so the
    caller resolves ids through the database instead of through login.
    """

    response = await client.post("/v1/auth/register", json={"email": email, "password": password})
    if response.status_code == 201:
        return str(response.json()["data"]["id"])
    if response.status_code != 409:
        response.raise_for_status()
    return None


async def main() -> None:
    api_url = os.environ["E2E_API_URL"]
    finance_email = os.environ["E2E_FINANCE_EMAIL"]
    disabled_email = os.environ["E2E_DISABLED_EMAIL"]
    # A cold API can need several seconds for its first password hash even after
    # healthz is ready. This fixture is a readiness gate, not a 2-second SLA.
    async with httpx.AsyncClient(base_url=api_url, timeout=15, trust_env=False) as client:
        for _ in range(60):
            try:
                if (await client.get("/healthz")).status_code == 200:
                    break
            except httpx.HTTPError:
                pass
            await asyncio.sleep(1)
        else:
            raise RuntimeError("E2E API did not become ready")
        finance_id = await _register(
            client,
            email=finance_email,
            password=os.environ["E2E_FINANCE_PASSWORD"],
        )
        disabled_id = await _register(
            client,
            email=disabled_email,
            password=os.environ["E2E_DISABLED_PASSWORD"],
        )

    database_url = os.environ["DATABASE_URL"].replace("postgresql+asyncpg://", "postgresql://", 1)
    connection = await asyncpg.connect(database_url)
    try:
        finance_id = finance_id or await connection.fetchval(
            "SELECT id FROM users WHERE email = $1", finance_email
        )
        disabled_id = disabled_id or await connection.fetchval(
            "SELECT id FROM users WHERE email = $1", disabled_email
        )
        if finance_id is None or disabled_id is None:
            raise RuntimeError("E2E fixture accounts were not created")
        await connection.execute(
            "INSERT INTO user_roles (user_id, role) VALUES ($1::uuid, 'finance_operator') ON CONFLICT DO NOTHING",
            finance_id,
        )
        await connection.execute("UPDATE users SET status = 'disabled' WHERE id = $1::uuid", disabled_id)
    finally:
        await connection.close()


if __name__ == "__main__":
    asyncio.run(main())
