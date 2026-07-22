from __future__ import annotations

import os
from dataclasses import dataclass
from functools import lru_cache
from uuid import UUID


def _bool(name: str, default: bool = False) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() not in {"", "0", "false", "off", "no"}


@dataclass(frozen=True, slots=True)
class Settings:
    environment: str
    database_url: str
    redis_url: str
    compute_base_url: str
    compute_service_key: str
    compute_node_id: str
    foundation_routes_enabled: bool
    foundation_subject_id: UUID
    outbox_poll_seconds: float
    outbox_lease_seconds: int
    outbox_batch_size: int

    @property
    def is_production(self) -> bool:
        return self.environment == "production"


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    environment = os.getenv("APP_ENV", "development").strip().lower()
    foundation_enabled = _bool("FOUNDATION_ROUTES_ENABLED", environment in {"development", "test"})
    if environment == "production" and foundation_enabled:
        raise RuntimeError("FOUNDATION_ROUTES_ENABLED must be false in production")
    return Settings(
        environment=environment,
        database_url=os.getenv(
            "DATABASE_URL", "postgresql+asyncpg://fractal:fractal@127.0.0.1:5432/fractal_platform"
        ),
        redis_url=os.getenv("REDIS_URL", "redis://127.0.0.1:6379/0"),
        compute_base_url=os.getenv("COMPUTE_BASE_URL", "http://127.0.0.1:18080").rstrip("/"),
        compute_service_key=os.getenv("COMPUTE_SERVICE_KEY", ""),
        compute_node_id=os.getenv("COMPUTE_NODE_ID", "local-compute-1"),
        foundation_routes_enabled=foundation_enabled,
        foundation_subject_id=UUID(os.getenv("FOUNDATION_SUBJECT_ID", "00000000-0000-0000-0000-000000000001")),
        outbox_poll_seconds=float(os.getenv("OUTBOX_POLL_SECONDS", "0.5")),
        outbox_lease_seconds=int(os.getenv("OUTBOX_LEASE_SECONDS", "30")),
        outbox_batch_size=int(os.getenv("OUTBOX_BATCH_SIZE", "10")),
    )

