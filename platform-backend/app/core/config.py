"""Typed application configuration."""

from functools import lru_cache

from pydantic import Field, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Runtime settings for the HTTP identity boundary."""

    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    app_env: str = "development"
    log_json: bool = False
    database_url: str
    session_secret: str
    session_cookie_secure: bool = False
    session_ttl_days: int = 30
    api_origin: str = "http://localhost:18000"
    cors_origins: str = "http://localhost:3000,http://localhost:5173"
    idempotency_lease_seconds: int = 30
    idempotency_ttl_hours: int = 24
    auth_login_rate_limit_per_minute: int = Field(default=10, ge=1, le=300)
    auth_register_rate_limit_per_minute: int = Field(default=5, ge=1, le=100)
    trust_request_id_header: bool = False
    redis_url: str = "redis://localhost:6379/0"
    compute_base_url: str = "http://localhost:8080"
    compute_service_key: str = ""
    compute_connect_timeout_seconds: float = Field(default=5.0, gt=0, le=30)
    compute_read_timeout_seconds: float = Field(default=60.0, gt=0, le=300)
    s3_endpoint_url: str = ""
    s3_public_endpoint_url: str = ""
    s3_bucket: str = "fractal-platform"
    s3_region: str = "us-east-1"
    s3_access_key_id: str = ""
    s3_secret_access_key: str = ""
    preview_max_width: int = Field(default=1024, ge=1, le=1024)
    preview_max_height: int = Field(default=1024, ge=1, le=1024)
    preview_max_pixels: int = Field(default=1_048_576, ge=1, le=1_048_576)
    preview_rate_limit_per_minute: int = Field(default=30, ge=1, le=600)
    render_quota_max_active: int = Field(default=3, ge=1, le=100)
    master_download_ttl_seconds: int = Field(default=300, ge=60, le=900)
    render_poll_interval_seconds: int = Field(default=3, ge=1, le=60)
    outbox_poll_interval_seconds: float = Field(default=1.0, gt=0, le=60)
    outbox_lease_seconds: int = Field(default=30, ge=1, le=300)
    outbox_max_attempts: int = Field(default=10, ge=1, le=100)
    outbox_claim_batch_size: int = Field(default=20, ge=1, le=100)
    outbox_schedule_interval_seconds: float = Field(default=30.0, gt=0, le=3600)
    outbox_backoff_base_seconds: int = Field(default=2, ge=1, le=300)
    outbox_backoff_max_seconds: int = Field(default=300, ge=1, le=3600)

    @property
    def trusted_origins(self) -> set[str]:
        return {self.api_origin, *(origin.strip() for origin in self.cors_origins.split(",") if origin.strip())}

    @model_validator(mode="after")
    def validate_production_security(self) -> "Settings":
        if self.app_env != "production":
            return self
        if not self.session_cookie_secure:
            raise ValueError("SESSION_COOKIE_SECURE must be true in production")
        if len(self.session_secret) < 32 or self.session_secret.startswith("dev-"):
            raise ValueError("SESSION_SECRET must be a non-development secret in production")
        if not self.api_origin.startswith("https://"):
            raise ValueError("API_ORIGIN must use HTTPS in production")
        if any(origin.startswith("http://localhost") for origin in self.trusted_origins):
            raise ValueError("CORS_ORIGINS must not include localhost in production")
        return self


@lru_cache
def get_settings() -> Settings:
    return Settings()
