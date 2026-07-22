"""Create render and outbox foundation tables."""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "0001_foundation"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "render_jobs",
        sa.Column("id", sa.Uuid(), primary_key=True),
        sa.Column("owner_id", sa.Uuid(), nullable=False),
        sa.Column("kind", sa.String(80), nullable=False),
        sa.Column("request_json", postgresql.JSONB(), nullable=False),
        sa.Column("status", sa.String(40), nullable=False),
        sa.Column("idempotency_key", sa.String(200), nullable=False),
        sa.Column("compute_node_id", sa.String(120)),
        sa.Column("compute_run_id", sa.String(200)),
        sa.Column("progress_percent", sa.Integer(), nullable=False),
        sa.Column("result_manifest_json", postgresql.JSONB()),
        sa.Column("error_code", sa.String(120)),
        sa.Column("error_message", sa.String(2000)),
        sa.Column("cancel_requested_at", sa.DateTime(timezone=True)),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("finished_at", sa.DateTime(timezone=True)),
        sa.UniqueConstraint("owner_id", "idempotency_key", name="uq_render_job_owner_idempotency"),
    )
    op.create_index("ix_render_jobs_owner_created", "render_jobs", ["owner_id", "created_at"])
    op.create_table(
        "quota_reservations",
        sa.Column("id", sa.Uuid(), primary_key=True),
        sa.Column("user_id", sa.Uuid(), nullable=False),
        sa.Column("render_job_id", sa.Uuid(), sa.ForeignKey("render_jobs.id", ondelete="CASCADE"), nullable=False, unique=True),
        sa.Column("quota_kind", sa.String(80), nullable=False),
        sa.Column("units", sa.Integer(), nullable=False),
        sa.Column("status", sa.String(30), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.create_index("ix_quota_user_status", "quota_reservations", ["user_id", "status"])
    op.create_table(
        "outbox_events",
        sa.Column("id", sa.Uuid(), primary_key=True),
        sa.Column("event_type", sa.String(100), nullable=False),
        sa.Column("schema_version", sa.Integer(), nullable=False),
        sa.Column("aggregate_type", sa.String(80), nullable=False),
        sa.Column("aggregate_id", sa.Uuid(), nullable=False),
        sa.Column("payload_json", postgresql.JSONB(), nullable=False),
        sa.Column("idempotency_key", sa.String(200), nullable=False, unique=True),
        sa.Column("status", sa.String(30), nullable=False),
        sa.Column("available_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("lease_until", sa.DateTime(timezone=True)),
        sa.Column("attempt_count", sa.Integer(), nullable=False),
        sa.Column("completed_at", sa.DateTime(timezone=True)),
        sa.Column("last_error", sa.String(2000)),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.create_index("ix_outbox_claim", "outbox_events", ["status", "available_at", "lease_until"])


def downgrade() -> None:
    op.drop_table("outbox_events")
    op.drop_table("quota_reservations")
    op.drop_table("render_jobs")

