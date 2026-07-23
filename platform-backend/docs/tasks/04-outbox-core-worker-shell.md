# T04 — M7 Transactional Outbox Core and Worker Shell

## Task description

Implement the shared durable asynchronous execution mechanism. Product transactions append versioned events in the same PostgreSQL transaction; one worker claims events with leases and dispatches to module-owned handlers. The worker must not contain render, media, payment, or payout business rules.

## Work scope

- app/outbox/{models.py,repository.py,service.py,worker.py}
- app/core/{db.py,logging.py,request_context.py}
- docker-compose.dev.yml, app/main.py
- tests/e2e/test_outbox.py

## Goal

Guarantee no durable background command is lost after its browser request commits, while accepting at-least-once delivery and safe retries.

## Acceptance criteria

- A business transaction either commits its outbox row with state changes or commits neither.
- Worker claims due rows using FOR UPDATE SKIP LOCKED, creates a short lease, increments attempts, and records done, retry, dead, and error metadata.
- Expired leases become claimable. Failures use bounded exponential backoff; events at max attempts become visible dead letters and are never silently discarded.
- Event uniqueness is (event_type, aggregate_id, idempotency_key); payload is versioned IDs/immutable inputs only, never sessions, passwords, service keys, or raw Alipay bodies.
- Worker owns the periodic scheduling pass: it finds due pending payment attempts and eligible asset/payout cleanup work, then idempotently appends or reschedules payment.reconcile.v1 and cleanup.expired.v1. Domain modules own selection predicates and handler effects; worker owns no domain transition.

## Specification source

Transaction And Consistency Policy (Outbox claim); M7. Outbox And Worker Module; Event routing; Operational rules; Final Source Layout; ER Diagram; State Machines And Database Checks.

## Dependencies

- T01 and T02.
- asyncio, SQLAlchemy 2 async, asyncpg, PostgreSQL row locking.
- T03 supplies causation/user context. T06, T07, T08, T11, T12, and T13 provide business handlers.
- Handler calls occur outside the producer transaction; each handler must lock its aggregate and be idempotent.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -c /tmp/outbox.cookie -H 'Content-Type: application/json' -d '{"email":"outbox@example.test","password":"correct-horse-01"}' http://localhost:8000/v1/auth/register
curl --noproxy '*' -sS -f -b /tmp/outbox.cookie -H 'Idempotency-Key: render-outbox-0001' -H 'Content-Type: application/json' -d '{"recipeId":"RECIPE_ID","output":{"kind":"image","format":"png","width":512,"height":512}}' http://localhost:8000/v1/render-jobs
docker compose -f docker-compose.dev.yml logs worker --tail=100
~~~

Execute this E2E scenario after T06 provides the public producer. Assert the render request returns 202 before any Compute call, the worker claims exactly one event across repeated polling, and a forced transient Compute-stub failure produces a later retry rather than duplicate job submission. Restart worker during a lease and assert the event completes once after lease expiry.

After T12/T13, use the E2E stub clock to advance a pending payment and expired cleanup deadline. Assert the worker creates one due reconciliation/cleanup event without a browser route, processes it once, and does not create a duplicate event during the next scheduling pass.

## Implementation plan

1. Add event DTO, repository methods for append, claim, complete, reschedule, lease release, and dead-letter transition.
2. Add transaction-aware OutboxService; callers pass the same AsyncSession.
3. Add one polling worker process with handler registry, request-correlation propagation, and a bounded periodic scheduling pass.
4. Define owner-supplied due-work readers for payment reconciliation and cleanup; append/reschedule events with stable idempotency keys.
5. Enforce event schema version and redacted stable error codes.
6. Add Compose worker command, graceful shutdown, and metrics/log points.
7. Keep event-specific state machine logic in the owning module.
