"""Add outbox lease ownership, dead-letter timestamp and causation correlation.

Revision ID: 20260723_0003
Revises: 20260723_0002
Create Date: 2026-07-23 02:00:00
"""

from alembic import op
import sqlalchemy as sa


revision = "20260723_0003"
down_revision = "20260723_0002"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("outbox_events", sa.Column("lease_owner", sa.String(120)))
    op.add_column("outbox_events", sa.Column("dead_at", sa.DateTime(timezone=True)))
    op.add_column("outbox_events", sa.Column("causation_request_id", sa.String(64)))
    op.create_index(
        "ix_outbox_events_claim_lease",
        "outbox_events",
        ["status", "available_at", "lease_until"],
    )


def downgrade() -> None:
    op.drop_index("ix_outbox_events_claim_lease", table_name="outbox_events")
    op.drop_column("outbox_events", "causation_request_id")
    op.drop_column("outbox_events", "dead_at")
    op.drop_column("outbox_events", "lease_owner")
