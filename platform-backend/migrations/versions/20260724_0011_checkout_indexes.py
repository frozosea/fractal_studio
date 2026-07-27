"""Add M5 checkout lookup indexes.

Revision ID: 20260724_0011
Revises: 20260724_0010
"""

from __future__ import annotations

from alembic import op


revision = "20260724_0011"
down_revision = "20260724_0010"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_index("ix_order_items_listing_order", "order_items", ["listing_id", "order_id"])
    op.create_index("ix_payment_attempts_order_expires", "payment_attempts", ["order_id", "expires_at"])


def downgrade() -> None:
    op.drop_index("ix_payment_attempts_order_expires", table_name="payment_attempts")
    op.drop_index("ix_order_items_listing_order", table_name="order_items")
