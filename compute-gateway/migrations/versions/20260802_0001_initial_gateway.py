"""create Compute Gateway routing tables

Revision ID: 20260802_0001
Revises:
Create Date: 2026-08-02 00:00:00
"""

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "20260802_0001"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "compute_nodes",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("node_key", sa.String(length=64), nullable=False),
        sa.Column("base_url", sa.Text(), nullable=False),
        sa.Column("state", sa.String(length=16), nullable=False, server_default="disabled"),
        sa.Column("max_durable_slots", sa.Integer(), nullable=False, server_default="1"),
        sa.Column("max_preview_slots", sa.Integer(), nullable=False, server_default="2"),
        sa.Column("capabilities_json", postgresql.JSONB(astext_type=sa.Text()), nullable=True),
        sa.Column("capabilities_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("last_healthy_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("last_assigned_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.CheckConstraint("state IN ('active','draining','offline','disabled')", name="ck_compute_nodes_state"),
        sa.CheckConstraint("max_durable_slots BETWEEN 1 AND 64", name="ck_compute_nodes_durable_slots"),
        sa.CheckConstraint("max_preview_slots BETWEEN 1 AND 64", name="ck_compute_nodes_preview_slots"),
        sa.PrimaryKeyConstraint("id"),
        sa.UniqueConstraint("base_url", name="uq_compute_nodes_base_url"),
        sa.UniqueConstraint("node_key", name="uq_compute_nodes_node_key"),
    )
    op.create_index("ix_compute_nodes_node_key", "compute_nodes", ["node_key"])
    op.create_index("ix_compute_nodes_state", "compute_nodes", ["state"])
    op.create_table(
        "compute_runs",
        sa.Column("gateway_run_id", sa.Uuid(), nullable=False),
        sa.Column("idempotency_key", sa.String(length=200), nullable=False),
        sa.Column("request_sha256", sa.String(length=64), nullable=False),
        sa.Column("node_id", sa.Uuid(), nullable=False),
        sa.Column("node_run_id", sa.String(length=256), nullable=True),
        sa.Column("kind", sa.String(length=64), nullable=False),
        sa.Column("state", sa.String(length=16), nullable=False, server_default="allocating"),
        sa.Column("request_json", postgresql.JSONB(astext_type=sa.Text()), nullable=False),
        sa.Column("last_status_json", postgresql.JSONB(astext_type=sa.Text()), nullable=True),
        sa.Column("reserved_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.Column("terminal_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.CheckConstraint("state IN ('allocating','queued','running','completed','failed','cancelled','node_lost')", name="ck_compute_runs_state"),
        sa.ForeignKeyConstraint(["node_id"], ["compute_nodes.id"]),
        sa.PrimaryKeyConstraint("gateway_run_id"),
        sa.UniqueConstraint("idempotency_key", name="uq_compute_runs_idempotency_key"),
        sa.UniqueConstraint("node_id", "node_run_id", name="uq_compute_runs_node_run"),
    )
    op.create_index("ix_compute_runs_idempotency_key", "compute_runs", ["idempotency_key"])
    op.create_index("ix_compute_runs_node_id", "compute_runs", ["node_id"])
    op.create_index("ix_compute_runs_state", "compute_runs", ["state"])
    op.create_table(
        "run_artifacts",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("gateway_run_id", sa.Uuid(), nullable=False),
        sa.Column("external_artifact_id", sa.String(length=512), nullable=False),
        sa.Column("node_artifact_id", sa.String(length=512), nullable=False),
        sa.Column("media_type", sa.String(length=128), nullable=False),
        sa.Column("size_bytes", sa.Integer(), nullable=False),
        sa.Column("sha256", sa.String(length=64), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.CheckConstraint("size_bytes BETWEEN 1 AND 524288000", name="ck_run_artifacts_size"),
        sa.ForeignKeyConstraint(["gateway_run_id"], ["compute_runs.gateway_run_id"]),
        sa.PrimaryKeyConstraint("id"),
        sa.UniqueConstraint("external_artifact_id", name="uq_run_artifacts_external_id"),
    )
    op.create_index("ix_run_artifacts_gateway_run_id", "run_artifacts", ["gateway_run_id"])
    op.create_table(
        "node_probes",
        sa.Column("id", sa.BigInteger(), autoincrement=True, nullable=False),
        sa.Column("node_id", sa.Uuid(), nullable=False),
        sa.Column("checked_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.Column("healthy", sa.Boolean(), nullable=False),
        sa.Column("latency_ms", sa.Integer(), nullable=True),
        sa.Column("error_code", sa.String(length=128), nullable=True),
        sa.ForeignKeyConstraint(["node_id"], ["compute_nodes.id"]),
        sa.PrimaryKeyConstraint("id"),
    )
    op.create_index("ix_node_probes_node_id", "node_probes", ["node_id"])


def downgrade() -> None:
    op.drop_table("node_probes")
    op.drop_table("run_artifacts")
    op.drop_table("compute_runs")
    op.drop_table("compute_nodes")
