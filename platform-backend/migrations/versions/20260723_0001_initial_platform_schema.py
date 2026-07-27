"""Create initial Platform schema from M1-M7 Domain and ER model.

Revision ID: 20260723_0001
Revises:
Create Date: 2026-07-23 00:00:00
"""

from __future__ import annotations

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "20260723_0001"
down_revision = None
branch_labels = None
depends_on = None


UUID = postgresql.UUID(as_uuid=True)
JSONB = postgresql.JSONB
MONEY = sa.Numeric(18, 2)
NOW = sa.text("now()")


def _enum(name: str, *values: str) -> postgresql.ENUM:
    return postgresql.ENUM(*values, name=name, create_type=False)


USER_STATUS = _enum("user_status", "active", "disabled")
USER_ROLE = _enum("user_role", "creator", "finance_operator")
IDEMPOTENCY_STATUS = _enum("idempotency_status", "processing", "completed")
ACTOR_TYPE = _enum("audit_actor_type", "user", "worker", "alipay", "system")
RENDER_JOB_STATUS = _enum(
    "render_job_status",
    "queued",
    "submitting",
    "running",
    "compute_succeeded",
    "ingesting",
    "completed",
    "failed",
    "cancel_requested",
    "cancelled",
)
RENDER_OUTPUT_KIND = _enum("render_output_kind", "image", "video", "hs_mesh", "transition_mesh")
QUOTA_STATUS = _enum("quota_reservation_status", "reserved", "released", "expired")
ASSET_STATUS = _enum("asset_status", "processing", "ready", "failed", "deleted")
ASSET_VISIBILITY = _enum("asset_visibility", "private", "hidden")
MEDIA_TYPE = _enum("media_type", "image", "video", "mesh")
ASSET_FILE_PURPOSE = _enum(
    "asset_file_purpose",
    "master",
    "thumbnail",
    "watermarked_preview",
    "video_poster",
    "render_manifest",
)
LISTING_STATUS = _enum("listing_status", "draft", "published", "unpublished", "archived")
ORDER_STATUS = _enum("order_status", "pending_payment", "fulfilled", "closed", "payment_exception")
PAYMENT_ATTEMPT_STATUS = _enum(
    "payment_attempt_status", "created", "pending", "succeeded", "closed", "failed"
)
ENTITLEMENT_STATUS = _enum("entitlement_status", "active", "revoked")
REFUND_REVERSAL_STATUS = _enum("refund_reversal_status", "detected", "applied", "manual_review")
LEDGER_ACCOUNT = _enum(
    "ledger_account", "creator_available", "creator_reserved", "platform_revenue"
)
LEDGER_ENTRY_TYPE = _enum(
    "ledger_entry_type",
    "creator_credit",
    "platform_fee",
    "creator_reversal",
    "platform_reversal",
    "payout_reserved",
    "payout_paid",
    "payout_released",
)
PAYOUT_STATUS = _enum("payout_request_status", "pending", "paid", "rejected", "cancelled")
OUTBOX_STATUS = _enum("outbox_status", "pending", "leased", "done", "dead")

ENUMS = (
    USER_STATUS,
    USER_ROLE,
    IDEMPOTENCY_STATUS,
    ACTOR_TYPE,
    RENDER_JOB_STATUS,
    RENDER_OUTPUT_KIND,
    QUOTA_STATUS,
    ASSET_STATUS,
    ASSET_VISIBILITY,
    MEDIA_TYPE,
    ASSET_FILE_PURPOSE,
    LISTING_STATUS,
    ORDER_STATUS,
    PAYMENT_ATTEMPT_STATUS,
    ENTITLEMENT_STATUS,
    REFUND_REVERSAL_STATUS,
    LEDGER_ACCOUNT,
    LEDGER_ENTRY_TYPE,
    PAYOUT_STATUS,
    OUTBOX_STATUS,
)


def _created_at() -> sa.Column:
    return sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW)


def _cny(column: str = "currency") -> sa.CheckConstraint:
    return sa.CheckConstraint(f"{column} = 'CNY'", name=f"ck_{column}_cny")


def upgrade() -> None:
    bind = op.get_bind()
    for enum in ENUMS:
        enum.create(bind, checkfirst=True)

    op.create_table(
        "users",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("email", sa.String(320), nullable=False),
        sa.Column("status", USER_STATUS, nullable=False, server_default="active"),
        sa.Column("password_hash", sa.String(255), nullable=False),
        _created_at(),
        sa.UniqueConstraint("email", name="uq_users_email"),
    )
    op.create_table(
        "user_roles",
        sa.Column("user_id", UUID, sa.ForeignKey("users.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("role", USER_ROLE, primary_key=True),
        sa.Column("granted_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW),
    )
    op.create_table(
        "creator_profiles",
        sa.Column("user_id", UUID, sa.ForeignKey("users.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("handle", sa.String(32), nullable=False),
        sa.Column("display_name", sa.String(120), nullable=False),
        sa.UniqueConstraint("handle", name="uq_creator_profiles_handle"),
        sa.CheckConstraint("handle = lower(handle)", name="ck_creator_profiles_handle_lowercase"),
    )
    op.create_table(
        "sessions",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("user_id", UUID, sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
        sa.Column("token_hash", sa.String(64), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("revoked_at", sa.DateTime(timezone=True)),
        _created_at(),
        sa.Column("created_ip_hash", sa.String(64), nullable=False),
        sa.Column("user_agent_hash", sa.String(64), nullable=False),
        sa.Column("rotated_from_session_id", UUID, sa.ForeignKey("sessions.id", ondelete="SET NULL")),
        sa.UniqueConstraint("token_hash", name="uq_sessions_token_hash"),
    )
    op.create_index("ix_sessions_active_lookup", "sessions", ["token_hash", "expires_at"])
    op.create_table(
        "idempotency_records",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("user_id", UUID, sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
        sa.Column("scope", sa.String(120), nullable=False),
        sa.Column("idempotency_key", sa.String(255), nullable=False),
        sa.Column("request_hash", sa.String(64), nullable=False),
        sa.Column("status", IDEMPOTENCY_STATUS, nullable=False, server_default="processing"),
        sa.Column("response_json", JSONB),
        sa.Column("response_status", sa.SmallInteger()),
        sa.Column("response_headers_json", JSONB),
        sa.Column("lease_owner", sa.String(120)),
        sa.Column("lease_until", sa.DateTime(timezone=True)),
        sa.Column("completed_at", sa.DateTime(timezone=True)),
        _created_at(),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.UniqueConstraint("user_id", "scope", "idempotency_key", name="uq_idempotency_scope_key"),
        sa.CheckConstraint(
            "(status <> 'completed') OR (completed_at IS NOT NULL AND response_status IS NOT NULL)",
            name="ck_idempotency_completed_response",
        ),
    )
    op.create_table(
        "audit_events",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("actor_user_id", UUID, sa.ForeignKey("users.id", ondelete="SET NULL")),
        sa.Column("actor_type", ACTOR_TYPE, nullable=False),
        sa.Column("action", sa.String(160), nullable=False),
        sa.Column("subject_type", sa.String(80), nullable=False),
        sa.Column("subject_id", UUID, nullable=False),
        sa.Column("metadata_json", JSONB, nullable=False, server_default=sa.text("'{}'::jsonb")),
        _created_at(),
        sa.CheckConstraint(
            "(actor_type = 'user') = (actor_user_id IS NOT NULL)",
            name="ck_audit_actor_identity",
        ),
    )
    op.create_index("ix_audit_events_subject", "audit_events", ["subject_type", "subject_id", "created_at"])

    op.create_table(
        "fractal_recipes",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("owner_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("canonical_spec", JSONB, nullable=False),
        sa.Column("spec_hash", sa.String(64), nullable=False),
        sa.Column("structure_version", sa.Integer(), nullable=False),
        sa.Column("renderer_version", sa.String(80), nullable=False),
        _created_at(),
        sa.UniqueConstraint("owner_id", "spec_hash", name="uq_recipes_owner_spec_hash"),
    )
    op.create_table(
        "render_jobs",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("owner_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("recipe_id", UUID, sa.ForeignKey("fractal_recipes.id"), nullable=False),
        sa.Column("status", RENDER_JOB_STATUS, nullable=False, server_default="queued"),
        sa.Column("idempotency_key", sa.String(255), nullable=False),
        sa.Column("compute_run_id", sa.String(128)),
        sa.Column("output_kind", RENDER_OUTPUT_KIND, nullable=False),
        sa.Column("output_spec_json", JSONB, nullable=False),
        sa.Column("mapping_version", sa.String(80), nullable=False),
        sa.Column("progress_percent", sa.SmallInteger(), nullable=False, server_default="0"),
        sa.Column("compute_result_json", JSONB),
        sa.Column("selected_artifact_ids_json", JSONB),
        sa.Column("error_code", sa.String(120)),
        _created_at(),
        sa.UniqueConstraint("owner_id", "idempotency_key", name="uq_render_jobs_owner_idempotency"),
        sa.CheckConstraint("progress_percent BETWEEN 0 AND 100", name="ck_render_jobs_progress"),
    )
    op.create_index("ix_render_jobs_owner_created", "render_jobs", ["owner_id", "created_at"])
    op.create_index("ix_render_jobs_status_created", "render_jobs", ["status", "created_at"])
    op.create_table(
        "quota_reservations",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("user_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("render_job_id", UUID, sa.ForeignKey("render_jobs.id"), nullable=False),
        sa.Column("quota_kind", sa.String(80), nullable=False),
        sa.Column("units", sa.Integer(), nullable=False),
        sa.Column("status", QUOTA_STATUS, nullable=False, server_default="reserved"),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.UniqueConstraint("render_job_id", name="uq_quota_reservations_render_job"),
        sa.CheckConstraint("units > 0", name="ck_quota_reservations_units_positive"),
    )

    op.create_table(
        "assets",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("owner_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("recipe_id", UUID, sa.ForeignKey("fractal_recipes.id"), nullable=False),
        sa.Column("render_job_id", UUID, sa.ForeignKey("render_jobs.id"), nullable=False),
        sa.Column("media_type", MEDIA_TYPE, nullable=False),
        sa.Column("status", ASSET_STATUS, nullable=False, server_default="processing"),
        sa.Column("visibility", ASSET_VISIBILITY, nullable=False, server_default="private"),
        _created_at(),
        sa.UniqueConstraint("render_job_id", name="uq_assets_render_job"),
    )
    op.create_index("ix_assets_owner_created", "assets", ["owner_id", "created_at"])
    op.create_table(
        "asset_files",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("asset_id", UUID, sa.ForeignKey("assets.id"), nullable=False),
        sa.Column("purpose", ASSET_FILE_PURPOSE, nullable=False),
        sa.Column("object_key", sa.String(1024), nullable=False),
        sa.Column("sha256", sa.String(64), nullable=False),
        sa.Column("size_bytes", sa.BigInteger(), nullable=False),
        sa.Column("media_type", sa.String(120), nullable=False),
        sa.UniqueConstraint("object_key", name="uq_asset_files_object_key"),
        sa.UniqueConstraint("asset_id", "purpose", name="uq_asset_files_asset_purpose"),
        sa.CheckConstraint("size_bytes > 0", name="ck_asset_files_size_positive"),
    )

    op.create_table(
        "listings",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("asset_id", UUID, sa.ForeignKey("assets.id"), nullable=False),
        sa.Column("creator_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("status", LISTING_STATUS, nullable=False, server_default="draft"),
        sa.Column("title", sa.String(120), nullable=False),
        sa.Column("description", sa.String(4000), nullable=False, server_default=""),
        sa.Column("price_amount", MONEY, nullable=False),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("current_published_version_id", UUID),
        sa.Column("published_at", sa.DateTime(timezone=True)),
        _cny(),
        sa.CheckConstraint("price_amount >= 0.01", name="ck_listings_price_minimum"),
    )
    op.create_index(
        "uq_listings_one_non_archived_asset",
        "listings",
        ["asset_id"],
        unique=True,
        postgresql_where=sa.text("status <> 'archived'"),
    )
    op.create_index("ix_listings_published_feed", "listings", ["published_at", "id"])
    op.create_table(
        "listing_versions",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("listing_id", UUID, sa.ForeignKey("listings.id"), nullable=False),
        sa.Column("version", sa.Integer(), nullable=False),
        sa.Column("snapshot_json", JSONB, nullable=False),
        sa.Column("published_at", sa.DateTime(timezone=True), nullable=False),
        sa.UniqueConstraint("listing_id", "version", name="uq_listing_versions_listing_version"),
        sa.CheckConstraint("version > 0", name="ck_listing_versions_positive"),
    )
    op.create_foreign_key(
        "fk_listings_current_published_version",
        "listings",
        "listing_versions",
        ["current_published_version_id"],
        ["id"],
    )
    op.create_table(
        "licence_offers",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("listing_id", UUID, sa.ForeignKey("listings.id"), nullable=False),
        sa.Column("code", sa.String(80), nullable=False),
        sa.Column("terms_version", sa.String(80), nullable=False),
        sa.Column("terms_json", JSONB, nullable=False),
        sa.Column("is_active", sa.Boolean(), nullable=False, server_default=sa.true()),
    )
    op.create_index(
        "uq_licence_offers_one_active_listing",
        "licence_offers",
        ["listing_id"],
        unique=True,
        postgresql_where=sa.text("is_active"),
    )
    op.create_table(
        "listing_tags",
        sa.Column("listing_id", UUID, sa.ForeignKey("listings.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("tag", sa.String(32), primary_key=True),
    )
    op.create_index("ix_listing_tags_tag", "listing_tags", ["tag"])
    op.create_table(
        "favorites",
        sa.Column("user_id", UUID, sa.ForeignKey("users.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("asset_id", UUID, sa.ForeignKey("assets.id", ondelete="CASCADE"), primary_key=True),
        _created_at(),
    )

    op.create_table(
        "orders",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("buyer_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("status", ORDER_STATUS, nullable=False, server_default="pending_payment"),
        sa.Column("amount", MONEY, nullable=False),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("paid_at", sa.DateTime(timezone=True)),
        _created_at(),
        _cny(),
        sa.CheckConstraint("amount >= 0.01", name="ck_orders_amount_minimum"),
    )
    op.create_index("ix_orders_buyer_created", "orders", ["buyer_id", "created_at"])
    op.create_table(
        "payment_attempts",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("order_id", UUID, sa.ForeignKey("orders.id"), nullable=False),
        sa.Column("out_trade_no", sa.String(64), nullable=False),
        sa.Column("alipay_trade_no", sa.String(64)),
        sa.Column("status", PAYMENT_ATTEMPT_STATUS, nullable=False, server_default="created"),
        sa.Column("amount", MONEY, nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.UniqueConstraint("order_id", name="uq_payment_attempts_order"),
        sa.UniqueConstraint("out_trade_no", name="uq_payment_attempts_out_trade_no"),
        sa.UniqueConstraint("alipay_trade_no", name="uq_payment_attempts_alipay_trade_no"),
        sa.CheckConstraint("amount >= 0.01", name="ck_payment_attempts_amount_minimum"),
    )
    op.create_table(
        "payment_notifications",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("payment_attempt_id", UUID, sa.ForeignKey("payment_attempts.id"), nullable=False),
        sa.Column("fingerprint", sa.String(64), nullable=False),
        sa.Column("trade_status", sa.String(40), nullable=False),
        sa.Column("payload_redacted", JSONB, nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW),
        sa.UniqueConstraint("fingerprint", name="uq_payment_notifications_fingerprint"),
    )
    op.create_table(
        "refund_reversals",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("order_id", UUID, sa.ForeignKey("orders.id"), nullable=False),
        sa.Column("payment_attempt_id", UUID, sa.ForeignKey("payment_attempts.id"), nullable=False),
        sa.Column("amount", MONEY, nullable=False),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("status", REFUND_REVERSAL_STATUS, nullable=False, server_default="detected"),
        sa.Column("external_reference", sa.String(160)),
        sa.Column("detected_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW),
        sa.Column("applied_at", sa.DateTime(timezone=True)),
        _cny(),
        sa.CheckConstraint("amount >= 0.01", name="ck_refund_reversals_amount_minimum"),
        sa.UniqueConstraint("payment_attempt_id", name="uq_refund_reversals_payment_attempt"),
    )
    op.create_table(
        "order_items",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("order_id", UUID, sa.ForeignKey("orders.id"), nullable=False),
        sa.Column("listing_id", UUID, sa.ForeignKey("listings.id"), nullable=False),
        sa.Column("listing_version_id", UUID, sa.ForeignKey("listing_versions.id"), nullable=False),
        sa.Column("licence_offer_id", UUID, sa.ForeignKey("licence_offers.id"), nullable=False),
        sa.Column("asset_id", UUID, sa.ForeignKey("assets.id"), nullable=False),
        sa.Column("creator_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("price_amount", MONEY, nullable=False),
        sa.Column("commission_policy_version", sa.String(80), nullable=False),
        sa.Column("creator_amount", MONEY, nullable=False),
        sa.Column("platform_fee_amount", MONEY, nullable=False),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("listing_snapshot_json", JSONB, nullable=False),
        sa.Column("licence_snapshot_json", JSONB, nullable=False),
        _cny(),
        sa.CheckConstraint("price_amount >= 0.01", name="ck_order_items_price_minimum"),
        sa.CheckConstraint("creator_amount >= 0", name="ck_order_items_creator_nonnegative"),
        sa.CheckConstraint("platform_fee_amount >= 0", name="ck_order_items_fee_nonnegative"),
        sa.CheckConstraint(
            "creator_amount + platform_fee_amount = price_amount",
            name="ck_order_items_split_matches_price",
        ),
    )
    op.create_table(
        "entitlements",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("user_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("asset_id", UUID, sa.ForeignKey("assets.id"), nullable=False),
        sa.Column("order_item_id", UUID, sa.ForeignKey("order_items.id"), nullable=False),
        sa.Column("status", ENTITLEMENT_STATUS, nullable=False, server_default="active"),
        sa.Column("granted_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW),
        sa.Column("revoked_at", sa.DateTime(timezone=True)),
        sa.UniqueConstraint("order_item_id", name="uq_entitlements_order_item"),
    )
    op.create_index("ix_entitlements_user_asset", "entitlements", ["user_id", "asset_id", "status"])

    op.create_table(
        "creator_balances",
        sa.Column("creator_id", UUID, sa.ForeignKey("users.id"), primary_key=True),
        sa.Column("available_amount", MONEY, nullable=False, server_default="0"),
        sa.Column("reserved_amount", MONEY, nullable=False, server_default="0"),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW),
        _cny(),
        sa.CheckConstraint("available_amount >= 0", name="ck_creator_balances_available_nonnegative"),
        sa.CheckConstraint("reserved_amount >= 0", name="ck_creator_balances_reserved_nonnegative"),
    )
    op.create_table(
        "payout_requests",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("creator_id", UUID, sa.ForeignKey("users.id"), nullable=False),
        sa.Column("operator_user_id", UUID, sa.ForeignKey("users.id")),
        sa.Column("amount", MONEY, nullable=False),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("qr_object_key", sa.String(1024), nullable=False),
        sa.Column("status", PAYOUT_STATUS, nullable=False, server_default="pending"),
        sa.Column("external_reference", sa.String(160)),
        sa.Column("rejection_reason", sa.String(1000)),
        _created_at(),
        sa.Column("paid_at", sa.DateTime(timezone=True)),
        sa.Column("rejected_at", sa.DateTime(timezone=True)),
        sa.Column("cancelled_at", sa.DateTime(timezone=True)),
        _cny(),
        sa.CheckConstraint("amount >= 0.01", name="ck_payout_requests_amount_minimum"),
        sa.CheckConstraint(
            "(status <> 'paid') OR (operator_user_id IS NOT NULL AND external_reference IS NOT NULL)",
            name="ck_payout_requests_paid_operator_reference",
        ),
        sa.CheckConstraint(
            "(status <> 'rejected') OR (operator_user_id IS NOT NULL AND rejection_reason IS NOT NULL)",
            name="ck_payout_requests_rejected_operator_reason",
        ),
    )
    op.create_index(
        "uq_payout_requests_one_pending_creator",
        "payout_requests",
        ["creator_id"],
        unique=True,
        postgresql_where=sa.text("status = 'pending'"),
    )
    op.create_table(
        "ledger_entries",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("creator_id", UUID, sa.ForeignKey("users.id")),
        sa.Column("order_item_id", UUID, sa.ForeignKey("order_items.id")),
        sa.Column("payout_request_id", UUID, sa.ForeignKey("payout_requests.id")),
        sa.Column("account", LEDGER_ACCOUNT, nullable=False),
        sa.Column("signed_amount", MONEY, nullable=False),
        sa.Column("currency", sa.String(3), nullable=False, server_default="CNY"),
        sa.Column("entry_type", LEDGER_ENTRY_TYPE, nullable=False),
        _created_at(),
        _cny(),
        sa.CheckConstraint("signed_amount <> 0", name="ck_ledger_entries_nonzero"),
        sa.CheckConstraint(
            "(order_item_id IS NOT NULL) <> (payout_request_id IS NOT NULL)",
            name="ck_ledger_entries_one_source",
        ),
        sa.CheckConstraint(
            "(account = 'platform_revenue' AND creator_id IS NULL) OR "
            "(account IN ('creator_available', 'creator_reserved') AND creator_id IS NOT NULL)",
            name="ck_ledger_entries_account_creator",
        ),
    )
    op.create_index("ix_ledger_entries_creator_created", "ledger_entries", ["creator_id", "created_at"])
    op.create_index(
        "uq_ledger_order_item_type",
        "ledger_entries",
        ["order_item_id", "entry_type"],
        unique=True,
        postgresql_where=sa.text("order_item_id IS NOT NULL"),
    )
    op.create_index(
        "uq_ledger_payout_request_type",
        "ledger_entries",
        ["payout_request_id", "entry_type"],
        unique=True,
        postgresql_where=sa.text("payout_request_id IS NOT NULL"),
    )

    op.create_table(
        "outbox_events",
        sa.Column("id", UUID, primary_key=True),
        sa.Column("event_type", sa.String(120), nullable=False),
        sa.Column("schema_version", sa.Integer(), nullable=False, server_default="1"),
        sa.Column("aggregate_type", sa.String(80), nullable=False),
        sa.Column("aggregate_id", UUID, nullable=False),
        sa.Column("payload_json", JSONB, nullable=False),
        sa.Column("idempotency_key", sa.String(255), nullable=False),
        sa.Column("status", OUTBOX_STATUS, nullable=False, server_default="pending"),
        sa.Column("available_at", sa.DateTime(timezone=True), nullable=False, server_default=NOW),
        sa.Column("lease_until", sa.DateTime(timezone=True)),
        sa.Column("attempt_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("last_error_code", sa.String(120)),
        sa.Column("last_error_at", sa.DateTime(timezone=True)),
        _created_at(),
        sa.Column("completed_at", sa.DateTime(timezone=True)),
        sa.UniqueConstraint("event_type", "aggregate_id", "idempotency_key", name="uq_outbox_event_aggregate_key"),
        sa.CheckConstraint("schema_version > 0", name="ck_outbox_events_schema_version"),
        sa.CheckConstraint("attempt_count >= 0", name="ck_outbox_events_attempt_count"),
    )
    op.create_index("ix_outbox_events_due", "outbox_events", ["status", "available_at"])


def downgrade() -> None:
    op.drop_index("ix_outbox_events_due", table_name="outbox_events")
    op.drop_table("outbox_events")
    op.drop_index("uq_ledger_order_item_type", table_name="ledger_entries")
    op.drop_index("uq_ledger_payout_request_type", table_name="ledger_entries")
    op.drop_index("ix_ledger_entries_creator_created", table_name="ledger_entries")
    op.drop_table("ledger_entries")
    op.drop_index("uq_payout_requests_one_pending_creator", table_name="payout_requests")
    op.drop_table("payout_requests")
    op.drop_table("creator_balances")
    op.drop_index("ix_entitlements_user_asset", table_name="entitlements")
    op.drop_table("entitlements")
    op.drop_table("order_items")
    op.drop_table("refund_reversals")
    op.drop_table("payment_notifications")
    op.drop_table("payment_attempts")
    op.drop_index("ix_orders_buyer_created", table_name="orders")
    op.drop_table("orders")
    op.drop_table("favorites")
    op.drop_index("ix_listing_tags_tag", table_name="listing_tags")
    op.drop_table("listing_tags")
    op.drop_index("uq_licence_offers_one_active_listing", table_name="licence_offers")
    op.drop_table("licence_offers")
    op.drop_constraint("fk_listings_current_published_version", "listings", type_="foreignkey")
    op.drop_table("listing_versions")
    op.drop_index("ix_listings_published_feed", table_name="listings")
    op.drop_index("uq_listings_one_non_archived_asset", table_name="listings")
    op.drop_table("listings")
    op.drop_table("asset_files")
    op.drop_index("ix_assets_owner_created", table_name="assets")
    op.drop_table("assets")
    op.drop_table("quota_reservations")
    op.drop_index("ix_render_jobs_status_created", table_name="render_jobs")
    op.drop_index("ix_render_jobs_owner_created", table_name="render_jobs")
    op.drop_table("render_jobs")
    op.drop_table("fractal_recipes")
    op.drop_index("ix_audit_events_subject", table_name="audit_events")
    op.drop_table("audit_events")
    op.drop_table("idempotency_records")
    op.drop_index("ix_sessions_active_lookup", table_name="sessions")
    op.drop_table("sessions")
    op.drop_table("creator_profiles")
    op.drop_table("user_roles")
    op.drop_table("users")

    bind = op.get_bind()
    for enum in reversed(ENUMS):
        enum.drop(bind, checkfirst=True)
