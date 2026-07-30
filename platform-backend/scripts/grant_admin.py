"""Grant the administrator role to an existing account.

The account must first be created through the normal registration flow. This
script deliberately does not accept or handle passwords.
"""

from __future__ import annotations

import argparse
import asyncio
import os

import asyncpg


def _database_url() -> str:
    value = os.environ.get("DATABASE_URL") or os.environ.get("MIGRATION_DATABASE_URL")
    if not value:
        raise RuntimeError("DATABASE_URL or MIGRATION_DATABASE_URL is required")
    return value.replace("postgresql+asyncpg://", "postgresql://", 1).replace(
        "postgresql+psycopg://", "postgresql://", 1
    )


async def grant(email: str) -> None:
    connection = await asyncpg.connect(_database_url())
    try:
        user = await connection.fetchrow(
            "SELECT id, email, status::text AS status FROM users WHERE email = $1", email
        )
        if user is None:
            raise RuntimeError(f"account does not exist: {email}")
        if user["status"] != "active":
            raise RuntimeError(f"account is not active: {email}")
        creator = await connection.fetchval(
            "SELECT EXISTS(SELECT 1 FROM creator_profiles WHERE user_id = $1)",
            user["id"],
        )
        if creator:
            raise RuntimeError(f"account is a creator and cannot become an administrator: {email}")
        await connection.execute(
            "INSERT INTO user_roles (user_id, role) VALUES ($1, 'admin') "
            "ON CONFLICT (user_id, role) DO NOTHING",
            user["id"],
        )
        print(f"administrator role granted: {user['email']} ({user['id']})")
    finally:
        await connection.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--email", default=os.environ.get("ADMIN_EMAIL"), required=False)
    args = parser.parse_args()
    if not args.email:
        parser.error("--email or ADMIN_EMAIL is required")
    asyncio.run(grant(args.email.strip().lower()))


if __name__ == "__main__":
    main()
