"""Add database-level creator handle validation.

Revision ID: 20260723_0002
Revises: 20260723_0001
Create Date: 2026-07-23 01:00:00
"""

from alembic import op


revision = "20260723_0002"
down_revision = "20260723_0001"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_check_constraint(
        "ck_creator_profiles_handle_format",
        "creator_profiles",
        "handle ~ '^[a-z0-9_]{3,32}$'",
    )


def downgrade() -> None:
    op.drop_constraint("ck_creator_profiles_handle_format", "creator_profiles", type_="check")
