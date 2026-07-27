# T02 — PostgreSQL Schema, Migrations, and Database Invariants

## Task description

Create the initial Alembic schema for persistent M1–M7 data. PostgreSQL is the source of truth: foreign keys, unique and partial-unique indexes, state checks, money checks, and indexes enforce cross-request invariants before services are implemented.

## Work scope

- migrations/versions/20260723_0001_initial_platform_schema.py, migrations/env.py
- app/{auth,studio,assets,marketplace,commerce,finance,outbox}/models.py
- app/core/{idempotency_repository.py,audit_repository.py,db.py}
- tests/e2e/test_schema_invariants.py

## Goal

Create every table and durable constraint in the ER diagram without making triggers own business transitions.

## Acceptance criteria

- A clean alembic upgrade head creates every ER-diagram entity, including audit, idempotency, quota, payment notification/reversal, ledger, payouts, and outbox; sessions include created_ip_hash, user_agent_hash, and rotated_from_session_id.
- Database constraints reject duplicate idempotency/favourites/jobs/events, a second active licence offer or pending payout, invalid state values, invalid money, invalid payout conditional fields, and illegal nullable audit actors.
- All monetary values use NUMERIC(18,2); order/listing/licence snapshots and ledger entries cannot be updated through application APIs.
- Migration is repeatable on a clean Compose volume and reports head.

## Specification source

Transaction And Consistency Policy; Session Mechanism; Domain Class Diagram; ER Diagram; Key constraints outside the visual notation; State Machines And Database Checks; Final Source Layout.

## Dependencies

- T01: Compose PostgreSQL, async DB lifecycle, Alembic.
- Alembic, SQLAlchemy 2, PostgreSQL JSONB, UUID, timestamptz, NUMERIC(18,2), pg_trgm.
- Services use READ COMMITTED, explicit row locks, and CAS; schema constraints do not replace those transactions.
- If application DB privileges permit update/delete, an append-only ledger trigger/policy blocks them; no trigger performs product state transitions.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -c /tmp/t02.cookie -H 'Content-Type: application/json' -d '{"email":"schema@example.test","password":"correct-horse-01"}' http://localhost:8000/v1/auth/register
curl --noproxy '*' -sS -o /tmp/t02-status -w '%{http_code}\n' -b /tmp/t02.cookie -H 'Idempotency-Key: recipe-schema-0001' -H 'Content-Type: application/json' -d '{"canonicalSpec":{"version":1}}' http://localhost:8000/v1/recipes
~~~

After dependent public routes exist, send concurrent API mutations: one creation and one replay only; same key with a different payload returns 409 idempotency_conflict. Build a ready listing and issue concurrent publish/payout requests: one succeeds, the competing request is 409. These cases prove constraints through API without host database tools.

## Implementation plan

1. Model every M1–M7 table, FK, ownership column, and polymorphic no-FK outbox event reference, including session fingerprint/rotation fields and audit actor-type nullability.
2. Add enum/CHECK constraints for states, amounts, CNY payout, ledger source, and payout conditional fields.
3. Add every named unique constraint from the specification.
4. Add partial unique indexes for active offers, non-archived asset listings, and pending payouts; add full-text, pg_trgm, and cursor indexes for catalogue reads.
5. Include migrations/script.py.mako as the Alembic template contract, add append-only ledger protection, and test the initial revision.
6. Never use a destructive production downgrade without an approved migration plan.
