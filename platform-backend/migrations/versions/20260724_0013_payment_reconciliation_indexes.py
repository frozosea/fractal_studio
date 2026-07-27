"""Add payment reconciliation indexes.

Revision ID: 20260724_0013
Revises: 20260724_0012
"""

from __future__ import annotations

from alembic import op


revision = "20260724_0013"
down_revision = "20260724_0012"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_index("ix_payment_attempts_status_expires", "payment_attempts", ["status", "expires_at"])
    op.create_index("ix_payment_notifications_attempt", "payment_notifications", ["payment_attempt_id"])


def downgrade() -> None:
    op.drop_index("ix_payment_notifications_attempt", table_name="payment_notifications")
    op.drop_index("ix_payment_attempts_status_expires", table_name="payment_attempts")
