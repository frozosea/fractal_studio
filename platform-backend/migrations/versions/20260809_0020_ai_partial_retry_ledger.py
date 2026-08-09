"""Preserve billable AI attempts as a durable, recoverable lifetime ledger.

Revision ID: 20260809_0020
Revises: 20260809_0019
"""
from alembic import op

revision = "20260809_0020"
down_revision = "20260809_0019"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute("""
    ALTER TABLE ai_requests DROP CONSTRAINT IF EXISTS ai_requests_status_check;
    UPDATE ai_requests
       SET status='partial'
     WHERE status='failed' AND first_output_at IS NOT NULL;
    ALTER TABLE ai_requests
      ADD CONSTRAINT ai_requests_status_check
      CHECK (status IN (
        'reserved','streaming','retrying','partial','completed','released','failed'
      ));

    ALTER TABLE ai_requests
      ADD COLUMN counts_toward_trial boolean,
      ADD COLUMN attempt_started_at timestamptz,
      ADD COLUMN lease_until timestamptz;

    -- 0020 is introduced before AI is enabled in production.  The membership
    -- backfill still gives any development rows the least surprising result;
    -- every request accepted after this migration writes the value explicitly.
    UPDATE ai_requests r
       SET counts_toward_trial = NOT EXISTS (
         SELECT 1 FROM memberships m
          WHERE m.user_id=r.owner_id AND m.status='active'
       );
    ALTER TABLE ai_requests
      ALTER COLUMN counts_toward_trial SET DEFAULT true,
      ALTER COLUMN counts_toward_trial SET NOT NULL;

    UPDATE ai_requests
       SET attempt_started_at=COALESCE(first_output_at,created_at),
           lease_until=now()
     WHERE status IN ('reserved','streaming','retrying');

    ALTER TABLE ai_requests
      DROP CONSTRAINT IF EXISTS ai_requests_conversation_id_fkey;
    ALTER TABLE ai_requests ALTER COLUMN conversation_id DROP NOT NULL;
    ALTER TABLE ai_requests
      ADD CONSTRAINT ai_requests_conversation_id_fkey
      FOREIGN KEY (conversation_id) REFERENCES ai_conversations(id) ON DELETE SET NULL;

    -- assistant_message_id is allocated before the message row exists.  It is
    -- an application-maintained stable identifier, rather than a premature FK.
    ALTER TABLE ai_requests
      DROP CONSTRAINT IF EXISTS ai_requests_assistant_message_id_fkey;

    ALTER TABLE ai_requests
      ALTER COLUMN idempotency_key DROP NOT NULL,
      ALTER COLUMN request_hash DROP NOT NULL,
      ALTER COLUMN provider_request_id DROP NOT NULL;
    ALTER TABLE ai_requests
      DROP CONSTRAINT IF EXISTS ai_requests_owner_id_idempotency_key_key;
    CREATE UNIQUE INDEX uq_ai_requests_owner_idempotency_live
      ON ai_requests(owner_id,idempotency_key)
      WHERE idempotency_key IS NOT NULL;
    CREATE UNIQUE INDEX uq_ai_requests_assistant_message_live
      ON ai_requests(assistant_message_id)
      WHERE assistant_message_id IS NOT NULL;
    CREATE INDEX ix_ai_requests_expired_lease
      ON ai_requests(lease_until)
      WHERE status IN ('reserved','streaming','retrying');
    """)


def downgrade() -> None:
    raise RuntimeError(
        "20260809_0020 cannot be downgraded without deleting the lifetime AI ledger"
    )
