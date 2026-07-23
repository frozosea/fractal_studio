"""PostgreSQL engine, session and transaction boundary."""

from functools import lru_cache

from sqlalchemy.ext.asyncio import AsyncEngine, async_sessionmaker, create_async_engine

from app.core.config import get_settings


@lru_cache
def get_engine() -> AsyncEngine:
    """Create one async engine per process."""
    return create_async_engine(get_settings().database_url, pool_pre_ping=True)


@lru_cache
def get_session_factory() -> async_sessionmaker:
    """Optional ORM boundary; outbox writers still receive caller transaction explicitly."""
    return async_sessionmaker(get_engine(), expire_on_commit=False)
