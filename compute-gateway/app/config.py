"""Gateway runtime configuration."""

from __future__ import annotations

import json
from functools import lru_cache
from typing import Any

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    database_url: str
    compute_gateway_service_key: str = Field(min_length=16)
    compute_gateway_admin_key: str = Field(min_length=16)
    compute_upstream_service_key: str = Field(min_length=16)
    node_probe_interval_seconds: float = Field(default=5, gt=0, le=60)
    node_capabilities_refresh_seconds: float = Field(default=60, ge=5, le=3600)
    node_offline_after_seconds: float = Field(default=15, ge=5, le=600)
    create_connect_timeout_seconds: float = Field(default=2, gt=0, le=30)
    create_read_timeout_seconds: float = Field(default=15, gt=0, le=300)
    preview_timeout_seconds: float = Field(default=25, gt=0, le=60)
    preview_queue_timeout_seconds: float = Field(default=3, gt=0, le=30)
    artifact_read_timeout_seconds: float = Field(default=300, gt=0, le=900)
    compute_gateway_bootstrap_nodes_json: str = "[]"

    @property
    def bootstrap_nodes(self) -> list[dict[str, Any]]:
        try:
            value = json.loads(self.compute_gateway_bootstrap_nodes_json)
        except ValueError as error:
            raise ValueError("COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON must be valid JSON") from error
        if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
            raise ValueError("COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON must be an array of objects")
        return value


@lru_cache
def get_settings() -> Settings:
    return Settings()
