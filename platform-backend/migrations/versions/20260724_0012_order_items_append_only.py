"""Protect frozen checkout line items from mutation.

Revision ID: 20260724_0012
Revises: 20260724_0011
"""

from __future__ import annotations

from alembic import op


revision = "20260724_0012"
down_revision = "20260724_0011"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute(
        """
        CREATE FUNCTION prevent_order_item_mutation() RETURNS trigger AS $$
        BEGIN
          RAISE EXCEPTION 'order_items are immutable';
        END;
        $$ LANGUAGE plpgsql;
        """
    )
    op.execute(
        """
        CREATE TRIGGER trg_order_items_immutable
        BEFORE UPDATE OR DELETE ON order_items
        FOR EACH ROW EXECUTE FUNCTION prevent_order_item_mutation();
        """
    )


def downgrade() -> None:
    op.execute("DROP TRIGGER trg_order_items_immutable ON order_items")
    op.execute("DROP FUNCTION prevent_order_item_mutation()")
