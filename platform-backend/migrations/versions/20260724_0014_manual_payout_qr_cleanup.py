"""Track terminal manual-payout QR deletion.

Revision ID: 20260724_0014
Revises: 20260724_0013
"""

from __future__ import annotations

import sqlalchemy as sa
from alembic import op


revision = "20260724_0014"
down_revision = "20260724_0013"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("payout_requests", sa.Column("qr_deleted_at", sa.DateTime(timezone=True)))
    op.create_index(
        "ix_payout_requests_creator_created",
        "payout_requests",
        ["creator_id", "created_at", "id"],
    )


def downgrade() -> None:
    op.drop_index("ix_payout_requests_creator_created", table_name="payout_requests")
    op.drop_column("payout_requests", "qr_deleted_at")
