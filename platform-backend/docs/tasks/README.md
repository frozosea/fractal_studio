# Platform Backend Implementation Tasks

Each file is an independently executable implementation task. Complete tasks in dependency order shown in [dependency-graph.md](dependency-graph.md).

| ID | Task |
|---|---|
| T00 | Specification reconciliation and delivery contract |
| T01 | Application foundation, development stack, and observability |
| T02 | PostgreSQL schema, migrations, and database invariants |
| T03 | M1 authentication, opaque sessions, RBAC, and idempotency |
| T04 | M7 transactional outbox core and worker shell |
| T05 | M2 immutable recipes and bounded synchronous previews |
| T06 | M2 durable render jobs and Compute worker |
| T07 | M3 render-artifact ingestion and object storage |
| T08 | M3 media derivatives, private library, and protected downloads |
| T09 | M4 catalogue, listings, licences, and favourites |
| T10 | M6a immutable sale ledger and creator balance projection |
| T11 | M5 checkout, frozen order snapshots, and entitlements |
| T12 | M5/M7 Alipay webhook, reconciliation, and reversal handling |
| T13 | M6b manual creator payout requests |
| T14 | Full MVP end-to-end regression suite |
| T15 | Compute production contract prerequisite (external C++ delivery) |

Every test plan uses the complete docker-compose.dev.yml environment and HTTP API calls. E2E fixture seeding is limited to the T00-approved e2e profile for finance_operator and disabled-user identities; product-state assertions never use direct test SQL. Compute and Alipay use contract-compatible test doubles. A real Compute service may replace its stub only after its production contract gate passes.
