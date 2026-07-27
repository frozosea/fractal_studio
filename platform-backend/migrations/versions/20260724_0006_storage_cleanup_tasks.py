"""Add durable cleanup work for non-transactional S3 ingestion failures.

Revision ID: 20260724_0006
Revises: 20260724_0005
"""

from __future__ import annotations

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "20260724_0006"
down_revision = "20260724_0005"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "storage_cleanup_tasks",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("object_keys_json", postgresql.JSONB, nullable=False),
        sa.Column("status", sa.String(16), nullable=False, server_default="pending"),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
        sa.Column("completed_at", sa.DateTime(timezone=True)),
        sa.CheckConstraint("status IN ('pending', 'done')", name="ck_storage_cleanup_tasks_status"),
        sa.CheckConstraint("jsonb_array_length(object_keys_json) > 0", name="ck_storage_cleanup_tasks_nonempty"),
    )
    op.create_index("ix_storage_cleanup_tasks_pending", "storage_cleanup_tasks", ["created_at"], postgresql_where=sa.text("status = 'pending'"))


def downgrade() -> None:
    op.drop_index("ix_storage_cleanup_tasks_pending", table_name="storage_cleanup_tasks")
    op.drop_table("storage_cleanup_tasks")
