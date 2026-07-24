"""Protect M6 journal from mutation outside append-only inserts.

Revision ID: 20260724_0010
Revises: 20260724_0009
"""

from __future__ import annotations

from alembic import op


revision = "20260724_0010"
down_revision = "20260724_0009"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute(
        """
        CREATE FUNCTION prevent_ledger_entry_mutation() RETURNS trigger AS $$
        BEGIN
          RAISE EXCEPTION 'ledger_entries are append-only';
        END;
        $$ LANGUAGE plpgsql;
        """
    )
    op.execute(
        """
        CREATE TRIGGER trg_ledger_entries_append_only
        BEFORE UPDATE OR DELETE ON ledger_entries
        FOR EACH ROW EXECUTE FUNCTION prevent_ledger_entry_mutation();
        """
    )


def downgrade() -> None:
    op.execute("DROP TRIGGER trg_ledger_entries_append_only ON ledger_entries")
    op.execute("DROP FUNCTION prevent_ledger_entry_mutation()")
