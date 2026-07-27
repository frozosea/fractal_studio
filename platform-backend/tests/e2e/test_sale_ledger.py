"""M6a integration against real API identities and PostgreSQL journal transaction."""

from __future__ import annotations

import json
import os
import uuid

import httpx
import pytest
from sqlalchemy import text
from sqlalchemy.exc import DBAPIError
from sqlalchemy.ext.asyncio import create_async_engine

from app.finance.sale_ledger_writer import PostgresSaleLedgerWriter


pytestmark = pytest.mark.skipif(
    not (os.getenv("E2E_API_URL") and os.getenv("E2E_DATABASE_URL")),
    reason="set E2E_API_URL and E2E_DATABASE_URL",
)


async def _seed_frozen_order_item(*, creator_id: str, buyer_id: str) -> str:
    """M5 will own this creation in T11; M6a reads only its frozen row."""
    ids = {name: str(uuid.uuid4()) for name in ("recipe", "job", "asset", "listing", "version", "offer", "order", "item")}
    engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
    try:
        async with engine.begin() as connection:
            await connection.execute(
                text(
                    """
                    INSERT INTO fractal_recipes (id, owner_id, canonical_spec, spec_hash, structure_version, renderer_version)
                    VALUES (CAST(:id AS uuid), CAST(:owner_id AS uuid), CAST(:spec AS jsonb), :hash, 1, 'm6-e2e')
                    """
                ),
                {"id": ids["recipe"], "owner_id": creator_id, "spec": json.dumps({"version": 1}), "hash": uuid.uuid4().hex},
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO render_jobs
                        (id, owner_id, recipe_id, status, idempotency_key, compute_request_json, output_kind,
                         output_spec_json, mapping_version)
                    VALUES (CAST(:id AS uuid), CAST(:owner_id AS uuid), CAST(:recipe_id AS uuid), 'completed', :key,
                            CAST(:request AS jsonb), 'image', CAST(:output AS jsonb), 'm6-e2e')
                    """
                ),
                {
                    "id": ids["job"], "owner_id": creator_id, "recipe_id": ids["recipe"],
                    "key": uuid.uuid4().hex, "request": json.dumps({}), "output": json.dumps({}),
                },
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO assets (id, owner_id, recipe_id, render_job_id, media_type, status, visibility)
                    VALUES (CAST(:id AS uuid), CAST(:owner_id AS uuid), CAST(:recipe_id AS uuid), CAST(:job_id AS uuid),
                            'image', 'ready', 'private')
                    """
                ),
                {"id": ids["asset"], "owner_id": creator_id, "recipe_id": ids["recipe"], "job_id": ids["job"]},
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO listings (id, asset_id, creator_id, status, title, description, price_amount, currency)
                    VALUES (CAST(:id AS uuid), CAST(:asset_id AS uuid), CAST(:creator_id AS uuid), 'draft', 'Ledger item', '', 100, 'CNY')
                    """
                ),
                {"id": ids["listing"], "asset_id": ids["asset"], "creator_id": creator_id},
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO listing_versions (id, listing_id, version, snapshot_json, published_at)
                    VALUES (CAST(:id AS uuid), CAST(:listing_id AS uuid), 1, CAST(:snapshot AS jsonb), now())
                    """
                ),
                {"id": ids["version"], "listing_id": ids["listing"], "snapshot": json.dumps({})},
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO licence_offers (id, listing_id, code, terms_version, terms_json, is_active)
                    VALUES (CAST(:id AS uuid), CAST(:listing_id AS uuid), 'personal', 'v1', CAST(:terms AS jsonb), true)
                    """
                ),
                {"id": ids["offer"], "listing_id": ids["listing"], "terms": json.dumps({})},
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO orders (id, buyer_id, status, amount, currency)
                    VALUES (CAST(:id AS uuid), CAST(:buyer_id AS uuid), 'fulfilled', 100, 'CNY')
                    """
                ),
                {"id": ids["order"], "buyer_id": buyer_id},
            )
            await connection.execute(
                text(
                    """
                    INSERT INTO order_items
                        (id, order_id, listing_id, listing_version_id, licence_offer_id, asset_id, creator_id,
                         price_amount, commission_policy_version, creator_amount, platform_fee_amount, currency,
                         listing_snapshot_json, licence_snapshot_json)
                    VALUES
                        (CAST(:id AS uuid), CAST(:order_id AS uuid), CAST(:listing_id AS uuid), CAST(:version_id AS uuid),
                         CAST(:offer_id AS uuid), CAST(:asset_id AS uuid), CAST(:creator_id AS uuid),
                         100, 'm6-e2e-v1', 80, 20, 'CNY', CAST(:listing_snapshot AS jsonb), CAST(:licence_snapshot AS jsonb))
                    """
                ),
                {
                    "id": ids["item"], "order_id": ids["order"], "listing_id": ids["listing"],
                    "version_id": ids["version"], "offer_id": ids["offer"], "asset_id": ids["asset"],
                    "creator_id": creator_id, "listing_snapshot": json.dumps({}), "licence_snapshot": json.dumps({}),
                },
            )
    finally:
        await engine.dispose()
    return ids["item"]


@pytest.mark.asyncio
async def test_sale_and_reversal_are_append_only_and_idempotent(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    async with httpx.AsyncClient(base_url=e2e_api_url, trust_env=False) as creator, httpx.AsyncClient(
        base_url=e2e_api_url, trust_env=False
    ) as buyer:
        creator_registration = await creator.post(
            "/v1/auth/register", json={"email": f"ledger-creator-{suffix}@example.test", "password": "correct-horse-01"}
        )
        buyer_registration = await buyer.post(
            "/v1/auth/register", json={"email": f"ledger-buyer-{suffix}@example.test", "password": "correct-horse-01"}
        )
        assert creator_registration.status_code == 201 and buyer_registration.status_code == 201
        item_id = await _seed_frozen_order_item(
            creator_id=creator_registration.json()["data"]["id"], buyer_id=buyer_registration.json()["data"]["id"]
        )

    engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
    writer = PostgresSaleLedgerWriter()
    try:
        async with engine.begin() as connection:
            await writer.record_sale(connection, order_item_id=uuid.UUID(item_id), request_id_value="ledger-sale-e2e")
            await writer.record_sale(connection, order_item_id=uuid.UUID(item_id), request_id_value="ledger-sale-replay")
        async with engine.connect() as connection:
            sale_rows = await connection.execute(
                text("SELECT entry_type::text, signed_amount FROM ledger_entries WHERE order_item_id = CAST(:id AS uuid) ORDER BY entry_type"),
                {"id": item_id},
            )
            assert [(row[0], str(row[1])) for row in sale_rows] == [("creator_credit", "80.00"), ("platform_fee", "20.00")]
            balance = await connection.scalar(text("SELECT available_amount FROM creator_balances WHERE creator_id = (SELECT creator_id FROM order_items WHERE id = CAST(:id AS uuid))"), {"id": item_id})
            assert str(balance) == "80.00"

        async with engine.begin() as connection:
            await writer.reverse_sale(connection, order_item_id=uuid.UUID(item_id), request_id_value="ledger-reversal-e2e")
            await writer.reverse_sale(connection, order_item_id=uuid.UUID(item_id), request_id_value="ledger-reversal-replay")
        async with engine.connect() as connection:
            rows = await connection.execute(
                text("SELECT entry_type::text, signed_amount FROM ledger_entries WHERE order_item_id = CAST(:id AS uuid) ORDER BY entry_type"),
                {"id": item_id},
            )
            assert [(row[0], str(row[1])) for row in rows] == [
                ("creator_credit", "80.00"), ("creator_reversal", "-80.00"),
                ("platform_fee", "20.00"), ("platform_reversal", "-20.00"),
            ]
            balance = await connection.scalar(text("SELECT available_amount FROM creator_balances WHERE creator_id = (SELECT creator_id FROM order_items WHERE id = CAST(:id AS uuid))"), {"id": item_id})
            assert str(balance) == "0.00"
            audit = await connection.scalar(text("SELECT metadata_json FROM audit_events WHERE subject_id = CAST(:id AS uuid) AND action = 'ledger.sale_recorded'"), {"id": item_id})
            assert audit["requestId"] == "ledger-sale-e2e" and "secret" not in str(audit).lower()

        with pytest.raises(DBAPIError):
            async with engine.begin() as connection:
                await connection.execute(text("DELETE FROM ledger_entries WHERE order_item_id = CAST(:id AS uuid)"), {"id": item_id})
    finally:
        await engine.dispose()
