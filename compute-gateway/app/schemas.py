"""HTTP request and response schemas for Gateway-owned operations."""

from __future__ import annotations

import ipaddress
from datetime import datetime
from typing import Any

from pydantic import BaseModel, Field, HttpUrl, field_validator, model_validator


def blocked_literal_host(host: str) -> bool:
    """True when a literal address may never be used as a compute node target.

    Nodes live on the WireGuard mesh (10.66.0.0/24) or Docker networks, so
    loopback, link-local (incl. 169.254.169.254 metadata), multicast,
    unspecified, reserved and public literal addresses are never legitimate
    targets. Hostnames (Docker/WireGuard DNS) remain allowed.
    """
    try:
        addr = ipaddress.ip_address(host.lstrip("[").rstrip("]"))
    except ValueError:
        return False
    if addr.is_loopback or addr.is_link_local or addr.is_multicast or addr.is_unspecified:
        return True
    return not addr.is_private


class NodeUpsertInput(BaseModel):
    base_url: HttpUrl = Field(alias="baseUrl")
    max_durable_slots: int = Field(alias="maxDurableSlots", ge=1, le=64)
    max_preview_slots: int = Field(alias="maxPreviewSlots", ge=1, le=64)
    max_cpu_slots: int | None = Field(default=None, alias="maxCpuSlots", ge=1, le=64)
    max_gpu_slots: int | None = Field(default=None, alias="maxGpuSlots", ge=1, le=64)
    max_cpu_preview_slots: int | None = Field(default=None, alias="maxCpuPreviewSlots", ge=1, le=64)
    max_gpu_preview_slots: int | None = Field(default=None, alias="maxGpuPreviewSlots", ge=1, le=64)
    enabled: bool

    @field_validator("base_url")
    @classmethod
    def private_url_without_path(cls, value: HttpUrl) -> HttpUrl:
        if value.username or value.password or value.query or value.fragment:
            raise ValueError("baseUrl must not contain credentials, query, or fragment")
        if value.path not in {"", "/"}:
            raise ValueError("baseUrl must not contain a path")
        if blocked_literal_host(value.host):
            raise ValueError(
                "baseUrl host must be a private-network address or hostname; "
                "loopback, link-local, metadata, public and multicast addresses are blocked"
            )
        return value

    @model_validator(mode="after")
    def fill_resource_defaults(self) -> NodeUpsertInput:
        self.max_cpu_slots = self.max_cpu_slots or self.max_durable_slots
        self.max_gpu_slots = self.max_gpu_slots or self.max_durable_slots
        self.max_cpu_preview_slots = self.max_cpu_preview_slots or self.max_preview_slots
        self.max_gpu_preview_slots = self.max_gpu_preview_slots or self.max_preview_slots
        return self


class NodeView(BaseModel):
    node_key: str = Field(alias="nodeKey")
    state: str
    max_durable_slots: int = Field(alias="maxDurableSlots")
    used_durable_slots: int = Field(alias="usedDurableSlots")
    max_preview_slots: int = Field(alias="maxPreviewSlots")
    healthy: bool
    last_healthy_at: datetime | None = Field(alias="lastHealthyAt")


class ResourceAllocationView(BaseModel):
    used_slots: int = Field(alias="usedSlots")
    max_slots: int = Field(alias="maxSlots")
    used_preview_slots: int = Field(alias="usedPreviewSlots")
    max_preview_slots: int = Field(alias="maxPreviewSlots")


class ErrorBody(BaseModel):
    code: str
    message: str
    details: dict[str, Any]


class ErrorEnvelope(BaseModel):
    error: ErrorBody
