"""Persist exact versioned Compute DTO before durable submission."""

from __future__ import annotations

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "20260724_0004"
down_revision = "20260723_0003"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("render_jobs", sa.Column("compute_request_json", postgresql.JSONB(), nullable=True))
    op.execute("UPDATE render_jobs SET compute_request_json = '{}'::jsonb WHERE compute_request_json IS NULL")
    op.alter_column("render_jobs", "compute_request_json", nullable=False)


def downgrade() -> None:
    op.drop_column("render_jobs", "compute_request_json")
