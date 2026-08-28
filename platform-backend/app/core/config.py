"""Typed application configuration."""

from decimal import Decimal
from functools import lru_cache
from typing import Literal

from pydantic import Field, SecretStr, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Runtime settings for the HTTP identity boundary."""

    model_config = SettingsConfigDict(
        env_file=".env",
        extra="ignore",
        hide_input_in_errors=True,
    )

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
    compute_gateway_admin_key: str = ""
    compute_connect_timeout_seconds: float = Field(default=5.0, gt=0, le=30)
    compute_read_timeout_seconds: float = Field(default=60.0, gt=0, le=300)
    s3_endpoint_url: str = ""
    s3_public_endpoint_url: str = ""
    s3_bucket: str = "fractal-platform"
    s3_region: str = "us-east-1"
    s3_access_key_id: str = ""
    s3_secret_access_key: str = ""
    s3_server_side_encryption: str = ""
    s3_sse_kms_key_id: str = ""
    preview_max_width: int = Field(default=1024, ge=1, le=1024)
    preview_max_height: int = Field(default=1024, ge=1, le=1024)
    preview_max_pixels: int = Field(default=1_048_576, ge=1, le=1_048_576)
    preview_compute_timeout_seconds: float = Field(default=30.0, gt=0, le=60)
    preview_rate_limit_per_minute: int = Field(default=30, ge=1, le=600)
    preview_queue_max_pending: int = Field(default=10_000, ge=1, le=100_000)
    preview_cache_ttl_seconds: int = Field(default=60, ge=1, le=3600)
    # Jobs must outlive gateway retries. A short TTL turned a temporarily busy
    # Compute cluster into `preview_not_found` in the browser.
    preview_request_ttl_seconds: int = Field(default=600, ge=60, le=3600)
    preview_worker_concurrency: int = Field(default=4, ge=1, le=64)
    render_quota_max_active: int = Field(default=3, ge=1, le=100)
    # Lifetime export cap for accounts without an active membership. Members
    # are not counted against it.
    render_free_export_limit: int = Field(default=20, ge=1, le=10_000)
    master_download_ttl_seconds: int = Field(default=300, ge=60, le=900)
    public_preview_ttl_seconds: int = Field(default=3600, ge=60, le=86_400)
    media_max_input_bytes: int = Field(default=524_288_000, ge=1_048_576, le=2_147_483_647)
    media_max_derivative_bytes: int = Field(default=262_144_000, ge=1_048_576, le=2_147_483_647)
    media_ffmpeg_timeout_seconds: int = Field(default=120, ge=5, le=900)
    media_temp_dir: str = "/tmp"
    asset_cleanup_retry_delay_seconds: int = Field(default=300, ge=30, le=86_400)
    render_poll_interval_seconds: int = Field(default=3, ge=1, le=60)
    outbox_poll_interval_seconds: float = Field(default=1.0, gt=0, le=60)
    outbox_lease_seconds: int = Field(default=30, ge=1, le=300)
    outbox_max_attempts: int = Field(default=10, ge=1, le=100)
    outbox_claim_batch_size: int = Field(default=20, ge=1, le=100)
    outbox_schedule_interval_seconds: float = Field(default=30.0, gt=0, le=3600)
    outbox_backoff_base_seconds: int = Field(default=2, ge=1, le=300)
    outbox_backoff_max_seconds: int = Field(default=300, ge=1, le=3600)
    commission_policy_version: str = "mvp-v1"
    platform_fee_bps: int = Field(default=2000, ge=0, le=10_000)
    payment_attempt_ttl_minutes: int = Field(default=30, ge=1, le=1440)
    payment_reconcile_delay_seconds: int = Field(default=60, ge=0, le=3600)
    payment_reconcile_pending_seconds: int = Field(default=60, ge=5, le=3600)
    payment_reconcile_sweep_seconds: int = Field(default=300, ge=30, le=86_400)
    alipay_app_id: str = ""
    alipay_seller_id: str = ""
    alipay_private_key_path: str = ""
    alipay_public_key_path: str = ""
    alipay_notify_url: str = ""
    alipay_return_url: str = ""
    alipay_gateway_url: str = "https://openapi.alipay.com/gateway.do"
    alipay_stub_mode: bool = False
    alipay_stub_public_key_url: str = ""
    alipay_timeout_seconds: float = Field(default=10.0, gt=0, le=60)
    alipay_max_total_amount: Decimal = Field(default=Decimal("1_000_000.00"), ge=Decimal("0.01"))
    payout_qr_ttl_seconds: int = Field(default=600, ge=60, le=900)
    payout_qr_rejected_retention_days: int = Field(default=30, ge=1, le=3650)
    payout_qr_paid_retention_days: int = Field(default=90, ge=1, le=3650)
    payout_qr_cleanup_retry_delay_seconds: int = Field(default=300, ge=30, le=86_400)
    ai_enabled: bool = False
    ai_runtime_role: str = "api"
    # One active provider. SiliconFlow stays the default so existing deployments
    # upgrade without env changes; DeepSeek is selected with AI_PROVIDER=deepseek.
    ai_provider: Literal["siliconflow", "deepseek"] = "siliconflow"
    siliconflow_api_key: SecretStr = SecretStr("")
    siliconflow_base_url: str = "https://api.siliconflow.cn/v1"
    siliconflow_model: str = "Qwen/Qwen3.6-35B-A3B"
    deepseek_api_key: SecretStr = SecretStr("")
    deepseek_base_url: str = "https://api.deepseek.com"
    deepseek_model: str = "deepseek-chat"
    ai_free_lifetime_limit: int = Field(default=10, ge=1, le=1000)
    ai_history_ttl_days: int = Field(default=90, ge=1, le=3650)
    ai_max_user_message_chars: int = Field(default=4000, ge=1, le=20000)
    ai_max_output_tokens: int = Field(default=1500, ge=64, le=8192)
    ai_max_image_bytes: int = Field(default=1_048_576, ge=1024, le=10_485_760)
    ai_max_concurrent_per_user: int = Field(default=2, ge=1, le=10)

    @property
    def trusted_origins(self) -> set[str]:
        return {
            self.api_origin,
            *(origin.strip() for origin in self.cors_origins.split(",") if origin.strip()),
        }

    @model_validator(mode="after")
    def validate_production_security(self) -> "Settings":
        if self.ai_enabled and self.ai_runtime_role == "api":
            required_key = (
                self.deepseek_api_key if self.ai_provider == "deepseek" else self.siliconflow_api_key
            )
            required_env = (
                "DEEPSEEK_API_KEY" if self.ai_provider == "deepseek" else "SILICONFLOW_API_KEY"
            )
            if not reveal_secret(required_key):
                raise ValueError(f"{required_env} is required when AI_ENABLED=true")
        if self.app_env != "production":
            return self
        if not self.session_cookie_secure:
            raise ValueError("SESSION_COOKIE_SECURE must be true in production")
        if len(self.session_secret) < 32 or self.session_secret.startswith("dev-"):
            raise ValueError("SESSION_SECRET must be a non-development secret in production")
        # The admin compute monitoring is optional and degrades to 503 when the
        # key is absent (the API intentionally does not hold the Gateway ADMIN
        # key). Only enforce strength when a key is actually configured.
        if self.compute_gateway_admin_key and len(self.compute_gateway_admin_key) < 32:
            raise ValueError("COMPUTE_GATEWAY_ADMIN_KEY must be at least 32 characters in production")
        if not self.api_origin.startswith("https://"):
            raise ValueError("API_ORIGIN must use HTTPS in production")
        if any(origin.startswith("http://localhost") for origin in self.trusted_origins):
            raise ValueError("CORS_ORIGINS must not include localhost in production")
        if self.s3_server_side_encryption not in {"AES256", "aws:kms"}:
            raise ValueError("S3_SERVER_SIDE_ENCRYPTION must be AES256 or aws:kms in production")
        if self.s3_server_side_encryption == "aws:kms" and not self.s3_sse_kms_key_id:
            raise ValueError("S3_SSE_KMS_KEY_ID is required for aws:kms")
        if self.alipay_stub_mode:
            raise ValueError("ALIPAY_STUB_MODE must be false in production")
        if not all(
            (
                self.alipay_app_id,
                self.alipay_seller_id,
                self.alipay_private_key_path,
                self.alipay_public_key_path,
                self.alipay_notify_url,
                self.alipay_return_url,
            )
        ):
            raise ValueError("Alipay production configuration is required")
        if not self.alipay_notify_url.startswith("https://") or "?" in self.alipay_notify_url:
            raise ValueError(
                "ALIPAY_NOTIFY_URL must be a public HTTPS URL without query parameters"
            )
        return self


@lru_cache
def get_settings() -> Settings:
    return Settings()


def reveal_secret(value: SecretStr | str) -> str:
    """Reveal a configured secret only at the narrow outbound-call boundary.

    ``str(SecretStr)`` deliberately returns a mask. The temporary ``str`` branch
    keeps explicit contract-test settings compatible without weakening the
    production ``Settings`` type.
    """

    return value.get_secret_value() if isinstance(value, SecretStr) else value
