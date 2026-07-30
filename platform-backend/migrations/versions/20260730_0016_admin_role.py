"""Add the platform administrator role.

Revision ID: 20260730_0016
Revises: 20260729_0015
"""

from __future__ import annotations

from alembic import op


revision = "20260730_0016"
down_revision = "20260729_0015"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute("ALTER TYPE user_role ADD VALUE IF NOT EXISTS 'admin'")


def downgrade() -> None:
    # PostgreSQL cannot remove an enum value in place. Rebuild the type after
    # dropping administrator grants so a downgrade has deterministic semantics.
    op.execute("DELETE FROM user_roles WHERE role::text = 'admin'")
    op.execute("ALTER TYPE user_role RENAME TO user_role_with_admin")
    op.execute("CREATE TYPE user_role AS ENUM ('creator', 'finance_operator')")
    op.execute(
        "ALTER TABLE user_roles ALTER COLUMN role TYPE user_role "
        "USING role::text::user_role"
    )
    op.execute("DROP TYPE user_role_with_admin")
