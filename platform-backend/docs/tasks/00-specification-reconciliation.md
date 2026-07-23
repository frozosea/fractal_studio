# T00 — Specification Reconciliation and Delivery Contract

## Task description

Resolve internal specification contradictions before application implementation begins. Establish one canonical source layout, ownership boundaries, production dependency gates, and an E2E-only bootstrap contract so every later task can be implemented and verified without guessing.

## Work scope

- docs/platform-backend-spec.md
- docs/compute-openapi.yaml and docs/compute-spec.md where the Compute production gate is described
- docs/tasks/README.md and docs/tasks/dependency-graph.md
- docker-compose.dev.yml, .env.example, tests/conftest.py, scripts/e2e.sh

## Goal

Make the specification and task plan internally consistent and make every planned file, role, external contract, and scheduled event have one explicit owner.

## Acceptance criteria

- The canonical migration location is migrations/, not app/migrations/. The specification, Compose commands, and Alembic configuration agree.
- LicenceRegistry is owned by M4 at app/marketplace/licence_registry.py. The obsolete app/assets/licence_registry.py entry is removed from the final source layout and M4 file list is updated.
- tests/conftest.py is the shared test fixture entry point. tests/e2e/ contains scenario files and fixtures but does not replace the root conftest unless an explicit new layout is approved.
- The specification names the producer for delayed cleanup.expired.v1 events and periodic payment reconciliation; M7 worker owns scheduling/scanning, while M3/M5/M6 own handler business rules.
- A strictly E2E-profile-only bootstrap contract creates finance_operator and disabled-user fixtures before tests. It is disabled outside the e2e Compose profile, has no browser route, accepts no runtime production input, and never ships with production defaults.
- The external Compute gate names a separately owned C++ delivery: Bearer service auth, clientJobId uniqueness, request limits, standard errors, one cancel route, and artifact checksum/manifest support. Platform tasks do not claim this C++ work is complete.

## Specification source

Stack Map; M4. Catalogue Listing And Licence Module; Verified Current C++ Compute Contract And Required Production Contract; M7. Outbox And Worker Module; Final Source Layout; Public API; Deferred After MVP.

## Dependencies

- No preceding implementation task.
- Requires product-owner approval for canonical paths and the security review of E2E fixture seeding.
- T01–T14 depend on this decision record; it changes documentation/configuration only, not product behavior.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml --profile e2e up --build -d
curl --noproxy '*' -sS -f -c /tmp/operator.cookie -H 'Content-Type: application/json' -d '{"email":"operator.e2e@example.test","password":"e2e-only-password-01"}' http://localhost:8000/v1/auth/login
curl --noproxy '*' -sS -o /dev/null -w '%{http_code}\n' -c /tmp/disabled.cookie -H 'Content-Type: application/json' -d '{"email":"disabled.e2e@example.test","password":"e2e-only-password-01"}' http://localhost:8000/v1/auth/login
docker compose -f docker-compose.dev.yml --profile e2e config
~~~

Assert the seeded operator authenticates and may access finance-only test flows, the disabled fixture receives 401/403 according to the final auth error mapping, the normal development profile contains no E2E credentials or seed service, and every canonical path is present exactly once in the documentation.

## Implementation plan

1. Amend conflicting source-layout and migration references in the specification.
2. Record the M7 periodic scheduling owner and event payload/availability policy.
3. Define fixed E2E fixture identities through profile-scoped environment values; create them only in an isolated test database before API tests start.
4. Add a documentation check that rejects ambiguous canonical file paths and an E2E Compose check that rejects seed variables outside the e2e profile.
5. Link the Compute gate to the C++ owner/repository and keep Platform integration in stub mode until its acceptance evidence exists.
