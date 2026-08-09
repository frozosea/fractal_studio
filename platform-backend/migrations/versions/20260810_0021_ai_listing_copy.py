"""Persist replayable AI listing-copy candidates without storing preview images.

Revision ID: 20260810_0021
Revises: 20260809_0020
"""

from alembic import op


revision = "20260810_0021"
down_revision = "20260809_0020"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute(
        """
        CREATE TABLE ai_listing_copy_results (
          request_id uuid PRIMARY KEY
            REFERENCES ai_requests(id) ON DELETE CASCADE,
          owner_id uuid NOT NULL
            REFERENCES users(id) ON DELETE CASCADE,
          listing_id uuid NOT NULL
            REFERENCES listings(id) ON DELETE CASCADE,
          source_request_id uuid
            REFERENCES ai_listing_copy_results(request_id) ON DELETE SET NULL,
          locale varchar(2) NOT NULL CHECK (locale IN ('zh','en')),
          candidates jsonb NOT NULL CHECK (
            jsonb_typeof(candidates)='array' AND jsonb_array_length(candidates)=3
          ),
          created_at timestamptz NOT NULL DEFAULT now(),
          expires_at timestamptz NOT NULL
        );
        CREATE INDEX ix_ai_listing_copy_owner_listing_created
          ON ai_listing_copy_results(owner_id,listing_id,created_at DESC);
        CREATE INDEX ix_ai_listing_copy_expires
          ON ai_listing_copy_results(expires_at);
        """
    )


def downgrade() -> None:
    op.execute("DROP TABLE ai_listing_copy_results")
