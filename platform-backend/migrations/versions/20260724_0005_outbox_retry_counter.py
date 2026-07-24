"""Separate retry budget from normal outbox delivery/poll count.

Revision ID: 20260724_0005
Revises: 20260724_0004
Create Date: 2026-07-24 01:00:00
"""

from alembic import op
import sqlalchemy as sa


revision = "20260724_0005"
down_revision = "20260724_0004"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("outbox_events", sa.Column("retry_count", sa.Integer(), nullable=False, server_default="0"))
    op.create_check_constraint("ck_outbox_events_retry_count", "outbox_events", "retry_count >= 0")


def downgrade() -> None:
    op.drop_constraint("ck_outbox_events_retry_count", "outbox_events", type_="check")
    op.drop_column("outbox_events", "retry_count")
