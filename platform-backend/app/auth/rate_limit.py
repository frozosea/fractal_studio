"""Fail-closed Redis limits for unauthenticated password endpoints."""

from __future__ import annotations

import hashlib

from fastapi import HTTPException, Request, status
from redis import RedisError
from redis.asyncio import Redis

from app.core.config import Settings, get_settings


class AuthRateLimitUnavailable(RuntimeError):
    pass


class AuthRateLimiter:
    _INCREMENT_WITH_TTL = """
    local count = redis.call('INCR', KEYS[1])
    if count == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end
    return count
    """

    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    async def enforce(self, *, action: str, email: str, request: Request) -> None:
        limit = (
            self._settings.auth_login_rate_limit_per_minute
            if action == "login"
            else self._settings.auth_register_rate_limit_per_minute
        )
        ip = request.client.host if request.client else "unknown"
        fingerprint = hashlib.sha256(f"{email.lower()}|{ip}".encode()).hexdigest()
        client: Redis = Redis.from_url(self._settings.redis_url, decode_responses=True)
        try:
            count = int(
                await client.eval(self._INCREMENT_WITH_TTL, 1, f"auth-rate:v1:{action}:{fingerprint}", 60)
            )
        except (RedisError, OSError, ValueError) as error:
            raise AuthRateLimitUnavailable from error
        finally:
            await client.aclose()
        if count > limit:
            raise HTTPException(status_code=status.HTTP_429_TOO_MANY_REQUESTS, detail="auth_rate_limited")
