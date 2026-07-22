from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import Any

from sqlalchemy import DateTime, ForeignKey, Index, Integer, String, UniqueConstraint, Uuid
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import Mapped, mapped_column

from app.core.db import Base


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class RenderJob(Base):
    __tablename__ = "render_jobs"
    __table_args__ = (
        UniqueConstraint("owner_id", "idempotency_key", name="uq_render_job_owner_idempotency"),
        Index("ix_render_jobs_owner_created", "owner_id", "created_at"),
    )

    id: Mapped[uuid.UUID] = mapped_column(Uuid, primary_key=True, default=uuid.uuid4)
    owner_id: Mapped[uuid.UUID] = mapped_column(Uuid, nullable=False)
    kind: Mapped[str] = mapped_column(String(80), nullable=False)
    request_json: Mapped[dict[str, Any]] = mapped_column(JSONB, nullable=False)
    status: Mapped[str] = mapped_column(String(40), nullable=False, default="queued")
    idempotency_key: Mapped[str] = mapped_column(String(200), nullable=False)
    compute_node_id: Mapped[str | None] = mapped_column(String(120))
    compute_run_id: Mapped[str | None] = mapped_column(String(200))
    progress_percent: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    result_manifest_json: Mapped[dict[str, Any] | None] = mapped_column(JSONB)
    error_code: Mapped[str | None] = mapped_column(String(120))
    error_message: Mapped[str | None] = mapped_column(String(2000))
    cancel_requested_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False, default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False, default=utcnow)
    finished_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class QuotaReservation(Base):
    __tablename__ = "quota_reservations"
    __table_args__ = (Index("ix_quota_user_status", "user_id", "status"),)

    id: Mapped[uuid.UUID] = mapped_column(Uuid, primary_key=True, default=uuid.uuid4)
    user_id: Mapped[uuid.UUID] = mapped_column(Uuid, nullable=False)
    render_job_id: Mapped[uuid.UUID] = mapped_column(
        Uuid, ForeignKey("render_jobs.id", ondelete="CASCADE"), nullable=False, unique=True
    )
    quota_kind: Mapped[str] = mapped_column(String(80), nullable=False, default="render_job")
    units: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    status: Mapped[str] = mapped_column(String(30), nullable=False, default="reserved")
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False, default=utcnow)

