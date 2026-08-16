"""Add owner-scoped Studio AI conversations, requests and feedback.

Revision ID: 20260809_0019
Revises: 20260730_0018
"""
from alembic import op

revision = "20260809_0019"
down_revision = "20260730_0018"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute("""
    CREATE TABLE ai_conversations (
      id uuid PRIMARY KEY, owner_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      title varchar(120) NOT NULL DEFAULT '新对话', optimization_consent boolean NOT NULL DEFAULT false,
      created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now()
    );
    CREATE INDEX ix_ai_conversations_owner_updated ON ai_conversations(owner_id, updated_at DESC);
    CREATE TABLE ai_messages (
      id uuid PRIMARY KEY, conversation_id uuid NOT NULL REFERENCES ai_conversations(id) ON DELETE CASCADE,
      role varchar(16) NOT NULL CHECK (role IN ('user','assistant')),
      content text NOT NULL, suggestion jsonb, created_at timestamptz NOT NULL DEFAULT now()
    );
    CREATE INDEX ix_ai_messages_conversation_created ON ai_messages(conversation_id, created_at);
    CREATE TABLE ai_requests (
      id uuid PRIMARY KEY, owner_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      conversation_id uuid NOT NULL REFERENCES ai_conversations(id) ON DELETE CASCADE,
      user_message_id uuid REFERENCES ai_messages(id) ON DELETE SET NULL,
      assistant_message_id uuid REFERENCES ai_messages(id) ON DELETE SET NULL,
      idempotency_key varchar(200) NOT NULL, status varchar(16) NOT NULL,
      request_hash char(64) NOT NULL,
      first_output_at timestamptz, completed_at timestamptz, provider_request_id varchar(200),
      created_at timestamptz NOT NULL DEFAULT now(),
      UNIQUE(owner_id, idempotency_key),
      CHECK (status IN ('reserved','streaming','completed','released','failed'))
    );
    CREATE INDEX ix_ai_requests_owner_status ON ai_requests(owner_id, status);
    CREATE TABLE ai_feedback (
      id uuid PRIMARY KEY, message_id uuid NOT NULL UNIQUE REFERENCES ai_messages(id) ON DELETE CASCADE,
      owner_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      rating smallint NOT NULL CHECK (rating IN (-1,1)), consent boolean NOT NULL DEFAULT false,
      created_at timestamptz NOT NULL DEFAULT now()
    );
    """)


def downgrade() -> None:
    op.execute("DROP TABLE ai_feedback; DROP TABLE ai_requests; DROP TABLE ai_messages; DROP TABLE ai_conversations;")
