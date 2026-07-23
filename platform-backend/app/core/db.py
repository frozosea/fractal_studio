"""PostgreSQL engine, session and transaction boundary."""

from functools import lru_cache

from sqlalchemy.ext.asyncio import AsyncEngine, create_async_engine

from app.core.config import get_settings


@lru_cache
def get_engine() -> AsyncEngine:
    """Create one async engine per process."""
    return create_async_engine(get_settings().database_url, pool_pre_ping=True)
