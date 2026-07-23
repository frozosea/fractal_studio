"""Typed application configuration."""

from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Runtime settings for the HTTP identity boundary."""

    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    app_env: str = "development"
    database_url: str
    session_secret: str
    session_cookie_secure: bool = False
    session_ttl_days: int = 30
    api_origin: str = "http://localhost:18000"
    cors_origins: str = "http://localhost:3000,http://localhost:5173"
    idempotency_lease_seconds: int = 30
    idempotency_ttl_hours: int = 24

    @property
    def trusted_origins(self) -> set[str]:
        return {self.api_origin, *(origin.strip() for origin in self.cors_origins.split(",") if origin.strip())}


@lru_cache
def get_settings() -> Settings:
    return Settings()
