# Task Dependency Graph

Arrows mean “finish prerequisite first”. The implementation order is:

T00 -> T01 -> T02 -> T03 -> T04 -> T05 -> T06 -> T07 -> T08 -> T09 -> T10 -> T11 -> T12 -> T13 -> T14

T15 is a parallel external C++ delivery. T06/T07 may use the contract stub before T15, but real-Compute production acceptance is blocked until T15 completes.

Some tasks have additional direct prerequisites shown below.

~~~mermaid
flowchart LR
  T00[T00 Specification reconciliation]
  T01[T01 Foundation and dev stack]
  T02[T02 Schema and migrations]
  T03[T03 Auth, sessions, RBAC, idempotency]
  T04[T04 Outbox core and worker shell]
  T05[T05 Recipes and preview]
  T06[T06 Durable render jobs]
  T07[T07 Artifact ingest and storage]
  T08[T08 Media library and downloads]
  T09[T09 Catalogue and listings]
  T10[T10 Sale ledger core]
  T11[T11 Checkout and entitlements]
  T12[T12 Alipay settlement and reconciliation]
  T13[T13 Manual payouts]
  T14[T14 Full MVP E2E]
  T15[T15 Compute production contract]

  T00 --> T01
  T00 --> T14
  T01 --> T02
  T02 --> T03
  T02 --> T04
  T03 --> T04
  T02 --> T05
  T03 --> T05
  T04 --> T06
  T05 --> T06
  T06 --> T07
  T04 --> T07
  T07 --> T08
  T04 --> T08
  T08 --> T09
  T03 --> T09
  T09 --> T10
  T02 --> T10
  T03 --> T11
  T04 --> T11
  T09 --> T11
  T10 --> T11
  T11 --> T12
  T04 --> T12
  T10 --> T12
  T12 --> T13
  T10 --> T13
  T07 --> T13
  T03 --> T13
  T04 --> T13
  T01 --> T14
  T02 --> T14
  T03 --> T14
  T04 --> T14
  T05 --> T14
  T06 --> T14
  T07 --> T14
  T08 --> T14
  T09 --> T14
  T10 --> T14
  T11 --> T14
  T12 --> T14
  T13 --> T14
  T00 --> T15
  T15 -. real Compute production gate .-> T06
  T15 -. real Compute production gate .-> T07

  classDef foundation fill:#e8f2ff,stroke:#2563eb,color:#111827
  classDef product fill:#ecfdf5,stroke:#059669,color:#111827
  classDef commerce fill:#fff7ed,stroke:#ea580c,color:#111827
  class T00,T01,T02,T03,T04 foundation
  class T05,T06,T07,T08,T09 product
  class T10,T11,T12,T13,T14 commerce
  class T15 foundation
~~~

## Cross-task contracts

| Producer | Consumer | Contract and transaction boundary |
|---|---|---|
| T00 | all tasks | Canonical migration/test paths, M4 LicenceRegistry ownership, E2E-only finance/disabled fixture bootstrap, external Compute gate, and periodic event producer ownership. |
| T02 | all tasks | PostgreSQL schema, unique/partial-unique indexes, state CHECKs, money scale, foreign keys; services still use locked CAS. |
| T03 | T04–T14 | authenticated current user, creator/finance_operator RBAC, CSRF/Origin protection, audit context, idempotent mutation protocol. |
| T04 | T06–T13 | transactionally appended versioned event; leased at-least-once worker calls an idempotent aggregate handler after commit and performs periodic due-work scheduling. |
| T05 | T06 | immutable canonical recipe and pure versioned Compute mapper. |
| T06 | T07 | completed render job, immutable output spec/mapping version, selected allowlisted artifact IDs through AssetIngestionPort. |
| T07 | T08 | processing/ready asset, private verified master and provenance; media.create_derivatives.v1 event. |
| T08 | T09 | AssetReader safe publishability/preview reads; no master or storage key passes to marketplace. |
| T09 | T11 | ListingSnapshotReader returns one published immutable listing/version/licence offer and Decimal price. |
| T10 | T12/T13 | append-only sale/payout journal plus locked creator balance projection. |
| T11 | T08/T12 | EntitlementReader boolean for download; pending order/payment and immutable order item. |
| T12 | T13 | settled creator credit or reversal/manual-review status before funds may be reserved. |
| T15 | T06/T07 | Real Compute auth, clientJobId replay, cancellation, and artifact integrity contract; E2E stub remains valid until production switch. |

## External gates

| Gate | Blocks | Evidence required |
|---|---|---|
| Compute production contract | T06/T07 real Compute mode | compute-openapi.yaml supports Bearer service auth, clientJobId uniqueness, limits, standard errors, one cancel route, artifact manifest/checksum contract. Use stub until then. |
| Object storage | T07/T08/T13 | Private/public prefixes, MinIO/S3 credentials, encryption policy, short signed GET URLs, lifecycle cleanup permissions. |
| Alipay merchant configuration | T11/T12 real gateway mode | RSA2 keys, app_id, seller_id, public HTTPS notify_url, page.pay/wap.pay enabled, documented query/close access. Use signed stub in E2E. |
| Finance operations policy | T13 | finance_operator assignment, QR scanning/retention policy, Merchant Portal transfer process, external reference format. |

## Critical consistency rules

- Never call Compute, S3, ffmpeg, or Alipay while holding a database transaction or row lock.
- Render creation, cancellation, checkout, and payout reservation commit their state and outbox event together.
- Worker delivery is at least once. Every handler locks its aggregate and is idempotent by entity state or merchant order number.
- Worker periodic scans only schedule due reconciliation/cleanup events; M3, M5, and M6 keep their business predicates and state changes.
- PostgreSQL is durable quota and business-state authority. Redis is only fail-closed preview rate limiting.
- Money is CNY Decimal/NUMERIC(18,2), never float. Browser return URL is never payment proof.
- Outbox payloads/logs contain no session token, password, private key, signed URL, QR object key, or raw Alipay notification.
