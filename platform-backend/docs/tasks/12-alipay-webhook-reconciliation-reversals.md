# T12 — M5/M7 Alipay Webhook, Reconciliation, and Reversals

## Task description

Implement authoritative Alipay notification handling and worker/scheduled reconciliation. Verify RSA2 and all merchant/order/amount fields, lock one payment attempt, settle once, grant entitlement and ledger entries, or close/reverse safely. Browser redirects remain non-authoritative.

## Work scope

- app/commerce/{router.py,service.py,repository.py,models.py}
- app/infrastructure/alipay/payment_gateway.py
- app/outbox/{worker.py,service.py}
- app/finance/sale_ledger_writer.py, app/assets/ports.py
- tests/e2e/test_alipay_settlement.py

## Goal

Turn a verified provider result into one durable commerce transition despite duplicate notifications, delayed delivery, worker retry, and uncertain/reversed payments.

## Acceptance criteria

- POST /v1/webhooks/alipay has no session dependency, verifies RSA2 excluding sign/sign_type, requires configured app_id, seller_id, exact out_trade_no and amount, non-empty trade_no, and returns literal success only after commit. All non-success browser/API failures use the shared error envelope and baseline code mapping.
- Only TRADE_SUCCESS and TRADE_FINISHED settle. Duplicate notification fingerprint is a no-op. Notification raw content is encrypted/redacted for audit.
- Settlement transaction locks PaymentAttempt and atomically marks fulfilled, grants one entitlement, and calls M6 sale ledger writer. It is idempotent by payment/order state.
- payment.reconcile.v1 and periodic sweep query Alipay by out_trade_no; pending reschedules, expired unpaid closes with trade.close, and paid reversal creates RefundReversal plus compensating ledger/entitlement action.
- Partial refund or creator already paid out is manual_review; it is never silently closed.

## Specification source

Transaction And Consistency Policy (Alipay notification/reconcile); M5 payment flow; Alipay API Contract Audit; M7 event routing/operational rules; State Machines; Public API webhook; Sequence Diagrams 3 and 6.

## Dependencies

- T04, T10, T11; T08 uses resulting entitlement.
- cryptography RSA2, httpx, configured merchant private key/Alipay public key, contract test gateway.
- Locks PaymentAttempt and Order at READ COMMITTED; processed notification fingerprint, entitlement/order item, and ledger constraints provide idempotency.
- T13 payout later consumes creator balance; reversal must flag already-paid-out balance for finance review.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -H 'Content-Type: application/x-www-form-urlencoded' --data 'SIGNED_TRADE_SUCCESS_FOR_OUT_TRADE_NO' http://localhost:8000/v1/webhooks/alipay
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie http://localhost:8000/v1/orders/ORDER_ID
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie -X POST http://localhost:8000/v1/assets/ASSET_ID/download-url
~~~

Use the Alipay stub to generate valid signatures and query results. Assert webhook body is exactly success, order becomes fulfilled, one entitlement grants download, and replayed webhook does not duplicate ledger/entitlement. Send invalid signature/app/seller/amount and expect rejection. Suppress webhook, let worker reconcile paid result, then test closed, pending retry, full reversal, partial reversal manual_review, and lease-retry behavior.

## Implementation plan

1. Implement isolated RSA2 gateway, canonical form verification, query, close, and redaction.
2. Add sessionless webhook router and strict validation/error response handling.
3. Implement locked settlement, notification fingerprint storage, entitlement grant, and ledger invocation.
4. Implement reconcile event/sweep and idempotent close/reversal state paths.
5. Add alarm-quality dead-letter/manual-review logs without raw provider payloads.
