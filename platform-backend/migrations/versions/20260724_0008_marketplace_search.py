"""Add M4 catalogue search extensions and indexes.

Revision ID: 20260724_0008
Revises: 20260724_0007
"""

from __future__ import annotations

from alembic import op
import sqlalchemy as sa


revision = "20260724_0008"
down_revision = "20260724_0007"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute("CREATE EXTENSION IF NOT EXISTS pg_trgm")
    op.create_index(
        "ix_listings_published_cursor",
        "listings",
        ["status", "published_at", "id"],
        postgresql_where=sa.text("status = 'published'"),
    )
    op.create_index(
        "ix_listings_published_price_cursor",
        "listings",
        ["status", "price_amount", "id"],
        postgresql_where=sa.text("status = 'published'"),
    )
    op.create_index("ix_listings_creator_status", "listings", ["creator_id", "status", "id"])
    op.execute(
        "CREATE INDEX ix_listings_catalogue_fts ON listings USING gin "
        "(to_tsvector('simple', coalesce(title, '') || ' ' || coalesce(description, '')))"
    )
    op.execute("CREATE INDEX ix_listings_title_trgm ON listings USING gin (title gin_trgm_ops)")
    op.execute("CREATE INDEX ix_creator_profiles_handle_trgm ON creator_profiles USING gin (handle gin_trgm_ops)")


def downgrade() -> None:
    op.execute("DROP INDEX IF EXISTS ix_creator_profiles_handle_trgm")
    op.execute("DROP INDEX IF EXISTS ix_listings_title_trgm")
    op.execute("DROP INDEX IF EXISTS ix_listings_catalogue_fts")
    op.drop_index("ix_listings_creator_status", table_name="listings")
    op.drop_index("ix_listings_published_price_cursor", table_name="listings")
    op.drop_index("ix_listings_published_cursor", table_name="listings")
