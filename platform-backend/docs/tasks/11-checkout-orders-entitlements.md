# T11 — M5 Checkout, Frozen Order Snapshots, and Entitlements

## Task description

Implement authenticated checkout creation, buyer order reads, purchase history, immutable order/item snapshots, one payment attempt per order, Alipay form descriptor generation, and the entitlement read port used by M3.

## Work scope

- app/commerce/{router.py,service.py,repository.py,models.py,ports.py}
- app/marketplace/ports.py, app/finance/sale_ledger_writer.py
- app/infrastructure/alipay/payment_gateway.py
- app/assets/ports.py
- tests/e2e/test_checkout.py

## Goal

Start a payment without trusting browser price/amount or browser return URL, then establish immutable data required for later authoritative settlement.

## Acceptance criteria

- POST /v1/checkout locks/reads one published offer and atomically creates pending_payment Order, frozen OrderItem snapshots/split, unique PaymentAttempt.out_trade_no, and delayed payment.reconcile.v1. The response uses the shared data envelope and never exposes payment keys or mutable internal snapshots.
- Browser sends listingId, licenceOfferId, and channel only; server reads CNY Decimal amount/terms from M4 and returns signed desktop page.pay or mobile wap.pay descriptor.
- One MVP order has exactly one payment attempt. Retry happens only after a prior unpaid order is closed.
- GET order and purchases allow buyer only. EntitlementReader returns only active access boolean to M3.
- Browser return_url never changes order, entitlement, or ledger state.

## Specification source

M5. Checkout Order And Entitlement Module; Responsibilities And Dependencies; Payment flow steps 1-5; Alipay API Contract Audit; Boundary rules; Public API M5; Checkout and entitlement sequence diagram; Protected download sequence diagram.

## Dependencies

- T02, T03, T04, T09, T10.
- SQLAlchemy transaction/row locks, Decimal, cryptography/httpx payment gateway.
- M4 ListingSnapshotReader returns published immutable offer; M6 SaleLedgerWriter is injected but called only at settlement in T12.
- T08 calls the M5 EntitlementReader port.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie -H 'Idempotency-Key: checkout-0001' -H 'Content-Type: application/json' -d '{"listingId":"LISTING_ID","licenceOfferId":"OFFER_ID","channel":"desktop_web"}' http://localhost:8000/v1/checkout
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie http://localhost:8000/v1/orders/ORDER_ID
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie http://localhost:8000/v1/me/purchases
~~~

Assert checkout 201 with pending order and signed form fields, no entitlement, frozen amount equals listing snapshot, and duplicate same key replays. Tamper any client amount field or select draft/unpublished offer: expect 422/409/404 as appropriate. Visit return_url simulation and verify order remains pending.

## Implementation plan

1. Define order/payment DTOs and safe immutable order views.
2. Implement M4 snapshot read and locked transaction creating order/item/payment/reconcile event.
3. Implement unique merchant order number and payment-form adapter for desktop/mobile contracts.
4. Implement buyer ownership reads, cursor history, and EntitlementReader.
5. Redact any payment payload/keys and map provider unavailability to 503.
