# T13 — M6b Manual Creator Payout Requests

## Task description

Implement manual creator settlement requests. A creator uploads a validated Alipay QR image and requests an available CNY balance amount. A finance_operator views a short private QR URL, performs the transfer in Alipay Merchant Portal, then marks paid or rejects. No automatic transfer API is used.

## Work scope

- app/finance/{payout_router.py,payout_operator_router.py,manual_payout_service.py,repository.py,models.py,cleanup_service.py,sale_ledger_writer.py}
- app/infrastructure/storage/object_storage.py
- app/core/access_middleware.py, app/outbox/{service.py,worker.py}
- tests/e2e/test_manual_payouts.py

## Goal

Reserve creator funds exactly once against private operator evidence, settle/release the reservation atomically, and protect QR images through their entire lifecycle.

## Acceptance criteria

- Creator endpoint accepts multipart amount and exactly one PNG/JPEG QR, validates CNY positive Decimal, <=2 MiB, strips metadata/scans it, and never returns QR key or operator reference.
- Service uploads to an unguessable encrypted private staging key before locking balance. Transaction appends payout_reserved, updates projection, and creates pending request; failed transaction queues orphan cleanup.
- Partial unique index permits one pending request per creator. Insufficient balance or a competing request fails without changing balance.
- finance_operator-only routes issue ten-minute QR URL, require externalReference for mark-paid, reason for reject, and atomically append payout_paid or payout_released/update balance/state.
- QR deletion is queued 30 days after rejected/cancelled and 90 after paid. QR never appears in public list, logs, or creator-visible URL.
- Multipart idempotency request hash is normalized amount/currency fields plus the streamed QR SHA-256. Mark-paid and reject append request-correlated audit events in their settlement transaction.

## Specification source

M6.1 Manual Creator Payout Request; Transaction And Consistency Policy (create/mark payout); ER Diagram constraints; State Machines; M7 cleanup event; Public API M6; Deferred After MVP.

## Dependencies

- T02, T03, T04, T07, T10, T12.
- FastAPI multipart streaming, Pillow/image validation and scanning service, boto3 private encrypted object storage, SQLAlchemy row locks.
- Ledger and creator balance projection transaction; T12 sale settlement creates available balance.
- Explicitly does not depend on or call Alipay transfer API; human Merchant Portal is external workflow.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/creator.cookie -H 'Idempotency-Key: payout-0001' -F 'amount=10.00' -F 'qrCode=@tests/fixtures/qr.png;type=image/png' http://localhost:8000/v1/me/payout-requests
curl --noproxy '*' -sS -f -b /tmp/creator.cookie http://localhost:8000/v1/me/payout-requests
curl --noproxy '*' -sS -f -b /tmp/operator.cookie 'http://localhost:8000/internal/v1/payout-requests?status=pending'
curl --noproxy '*' -sS -f -b /tmp/operator.cookie -H 'Idempotency-Key: payout-paid-0001' -H 'Content-Type: application/json' -d '{"externalReference":"merchant-transfer-001"}' http://localhost:8000/internal/v1/payout-requests/PAYOUT_ID/mark-paid
curl --noproxy '*' -sS -f -b /tmp/creator.cookie -H 'Idempotency-Key: payout-cancel-0001' -X POST http://localhost:8000/v1/me/payout-requests/PENDING_PAYOUT_ID/cancel
curl --noproxy '*' -sS -f -b /tmp/operator.cookie -H 'Idempotency-Key: payout-reject-0001' -H 'Content-Type: application/json' -d '{"reason":"invalid account evidence"}' http://localhost:8000/internal/v1/payout-requests/SECOND_PAYOUT_ID/reject
~~~

Seed creator balance through the paid checkout path. Assert creation 201, creator list omits QR/reference, operator list includes a short QR URL, paid 200 consumes reservation, rejection/cancellation releases it, and each replay is safe. Replay the creation key with a different QR byte stream and expect 409 idempotency_conflict. Attempt a second pending request, insufficient amount, wrong MIME/oversize file, non-operator settlement, cancel after paid, and direct public QR retrieval; assert documented failure/no exposure. Advance cleanup scheduler/stub clock and assert correct retention event and settlement audit entry.

## Implementation plan

1. Add multipart DTO and streamed QR validation, scan, metadata stripping, staged private upload, and a request-hash stream that includes QR SHA-256.
2. Implement locked balance reservation/payout request creation with journal/projection/audit writes and orphan cleanup event.
3. Implement creator list/cancel and operator list/private URL/mark-paid/reject routes with RBAC/idempotency.
4. Implement status/CHECK validation, payment/release journal transitions, and required paid/rejected audit writes.
5. Implement delayed QR cleanup through outbox with retention deadlines.
