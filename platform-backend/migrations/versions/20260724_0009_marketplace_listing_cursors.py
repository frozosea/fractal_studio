"""Add creation timestamp for creator-list cursor pagination.

Revision ID: 20260724_0009
Revises: 20260724_0008
"""

from __future__ import annotations

from alembic import op
import sqlalchemy as sa


revision = "20260724_0009"
down_revision = "20260724_0008"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("listings", sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")))
    op.create_index("ix_listings_creator_created_cursor", "listings", ["creator_id", "created_at", "id"])


def downgrade() -> None:
    op.drop_index("ix_listings_creator_created_cursor", table_name="listings")
    op.drop_column("listings", "created_at")
