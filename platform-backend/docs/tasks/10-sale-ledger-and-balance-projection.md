# T10 — M6a Immutable Sale Ledger and Creator Balance Projection

## Task description

Implement M6a, the internal append-only journal used by checkout settlement and reversal. It records creator credit/platform fee from frozen order item amounts and maintains a cached creator balance projection in the same transaction. It does not implement any Alipay transfer.

## Work scope

- app/finance/{sale_ledger_writer.py,repository.py,models.py}
- app/commerce/ports.py
- migrations/versions/ follow-up checks/indexes if needed
- tests/e2e/test_sale_ledger.py

## Goal

Make sale and reversal accounting reproducible from immutable journal entries, never from mutable listing prices or an external payout API.

## Acceptance criteria

- SaleLedgerWriter records creator_credit and platform_fee in the settlement transaction from frozen OrderItem values only; creator_amount + platform_fee_amount equals price_amount before insert. Settlement/reversal audit metadata is request-correlated and contains no payment secrets.
- Reversal appends creator_reversal/platform_reversal rather than updating old records.
- Ledger entries are append-only, use exactly one source reference (order item or payout request), and balance projection updates in the same transaction.
- Journal types support creator_credit, platform_fee, creator_reversal, platform_reversal, payout_reserved, payout_paid, and payout_released. No available_at field and no automatic payout method exist.

## Specification source

M6. MVP Sale Ledger Module (M6a Core); Responsibilities And Dependencies; ER Diagram key constraints; Transaction And Consistency Policy (notification/reconcile and payout); State Machines And Database Checks.

## Dependencies

- T02 and T09; T11 calls this writer during settlement.
- SQLAlchemy transaction, Decimal, LedgerRepository, frozen M5 order item.
- Append-only database protection from T02; payout journal operations added in T13.
- This is intentionally completed before T11 according to M1 -> M7 -> M2 -> M3 -> M4 -> M6a -> M5.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie -H 'Idempotency-Key: checkout-ledger-0001' -H 'Content-Type: application/json' -d '{"listingId":"LISTING_ID","licenceOfferId":"OFFER_ID","channel":"desktop_web"}' http://localhost:8000/v1/checkout
curl --noproxy '*' -sS -f -H 'Content-Type: application/x-www-form-urlencoded' --data 'STUB_SIGNED_SUCCESS_FIELDS' http://localhost:8000/v1/webhooks/alipay
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie http://localhost:8000/v1/orders/ORDER_ID
~~~

Execute after T11/T12 expose settlement. Assert one fulfilled order has frozen split matching price and exactly one settlement despite repeated webhook. Drive a contract-stub full reversal, assert entitlement revocation policy and compensating records; no duplicate sale/reversal occurs after repeated reconciliation.

## Implementation plan

1. Define repository inserts/queries and narrow SaleLedgerWriter port.
2. Validate frozen CNY Decimal split before writes.
3. Add atomic projection update with locked creator balance.
4. Add idempotent reversal path keyed by order item/reversal state.
5. Emit safe audit/log records; expose no public finance route in M6a.
