"""Track derivative readiness and make M3 cleanup work schedulable.

Revision ID: 20260724_0007
Revises: 20260724_0006
"""

from __future__ import annotations

from alembic import op
import sqlalchemy as sa


revision = "20260724_0007"
down_revision = "20260724_0006"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "assets",
        sa.Column("derivative_status", sa.String(16), nullable=False, server_default="pending"),
    )
    op.add_column("assets", sa.Column("derivative_error_code", sa.String(120)))
    op.add_column("assets", sa.Column("derivative_updated_at", sa.DateTime(timezone=True)))
    op.create_check_constraint(
        "ck_assets_derivative_status",
        "assets",
        "derivative_status IN ('pending', 'ready', 'failed')",
    )
    op.add_column(
        "storage_cleanup_tasks",
        sa.Column("available_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
    )
    op.add_column(
        "storage_cleanup_tasks",
        sa.Column("retry_generation", sa.Integer(), nullable=False, server_default="0"),
    )
    op.add_column("storage_cleanup_tasks", sa.Column("last_error_code", sa.String(120)))
    op.add_column("storage_cleanup_tasks", sa.Column("last_error_at", sa.DateTime(timezone=True)))
    op.create_check_constraint(
        "ck_storage_cleanup_tasks_retry_generation",
        "storage_cleanup_tasks",
        "retry_generation >= 0",
    )
    op.create_index(
        "ix_storage_cleanup_tasks_due",
        "storage_cleanup_tasks",
        ["available_at", "created_at"],
        postgresql_where=sa.text("status = 'pending'"),
    )


def downgrade() -> None:
    op.drop_index("ix_storage_cleanup_tasks_due", table_name="storage_cleanup_tasks")
    op.drop_constraint("ck_storage_cleanup_tasks_retry_generation", "storage_cleanup_tasks", type_="check")
    op.drop_column("storage_cleanup_tasks", "last_error_at")
    op.drop_column("storage_cleanup_tasks", "last_error_code")
    op.drop_column("storage_cleanup_tasks", "retry_generation")
    op.drop_column("storage_cleanup_tasks", "available_at")
    op.drop_constraint("ck_assets_derivative_status", "assets", type_="check")
    op.drop_column("assets", "derivative_updated_at")
    op.drop_column("assets", "derivative_error_code")
    op.drop_column("assets", "derivative_status")
