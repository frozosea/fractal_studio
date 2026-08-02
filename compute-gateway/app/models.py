"""Gateway routing persistence models."""

from __future__ import annotations

from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import DateTime, ForeignKey, Integer, String, Text, UniqueConstraint, func
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class ComputeNode(Base):
    __tablename__ = "compute_nodes"

    id: Mapped[UUID] = mapped_column(primary_key=True, default=uuid4)
    node_key: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    base_url: Mapped[str] = mapped_column(Text, unique=True)
    state: Mapped[str] = mapped_column(String(16), default="disabled", index=True)
    max_durable_slots: Mapped[int] = mapped_column(Integer, default=1)
    max_preview_slots: Mapped[int] = mapped_column(Integer, default=2)
    capabilities_json: Mapped[dict[str, object] | None] = mapped_column(JSONB, nullable=True)
    capabilities_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_healthy_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_assigned_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now()
    )


class ComputeRun(Base):
    __tablename__ = "compute_runs"
    __table_args__ = (
        UniqueConstraint("node_id", "node_run_id", name="uq_compute_runs_node_run"),
    )

    gateway_run_id: Mapped[UUID] = mapped_column(primary_key=True, default=uuid4)
    idempotency_key: Mapped[str] = mapped_column(String(200), unique=True, index=True)
    request_sha256: Mapped[str] = mapped_column(String(64))
    node_id: Mapped[UUID] = mapped_column(ForeignKey("compute_nodes.id"), index=True)
    node_run_id: Mapped[str | None] = mapped_column(String(256), nullable=True)
    kind: Mapped[str] = mapped_column(String(64))
    state: Mapped[str] = mapped_column(String(16), default="allocating", index=True)
    request_json: Mapped[dict[str, object]] = mapped_column(JSONB)
    last_status_json: Mapped[dict[str, object] | None] = mapped_column(JSONB, nullable=True)
    reserved_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    terminal_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now()
    )


class RunArtifact(Base):
    __tablename__ = "run_artifacts"
    __table_args__ = (UniqueConstraint("external_artifact_id", name="uq_run_artifacts_external_id"),)

    id: Mapped[UUID] = mapped_column(primary_key=True, default=uuid4)
    gateway_run_id: Mapped[UUID] = mapped_column(ForeignKey("compute_runs.gateway_run_id"), index=True)
    external_artifact_id: Mapped[str] = mapped_column(String(512))
    node_artifact_id: Mapped[str] = mapped_column(String(512))
    media_type: Mapped[str] = mapped_column(String(128))
    size_bytes: Mapped[int] = mapped_column(Integer)
    sha256: Mapped[str] = mapped_column(String(64))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())


class NodeProbe(Base):
    __tablename__ = "node_probes"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True)
    node_id: Mapped[UUID] = mapped_column(ForeignKey("compute_nodes.id"), index=True)
    checked_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    healthy: Mapped[bool] = mapped_column(default=False)
    latency_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    error_code: Mapped[str | None] = mapped_column(String(128), nullable=True)
