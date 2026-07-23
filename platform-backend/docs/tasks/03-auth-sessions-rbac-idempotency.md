# T03 — M1 Authentication, Opaque Sessions, RBAC, and Idempotency

## Task description

Implement registration, password login/logout, /me, creator profile, server-side opaque sessions, access middleware, audit events, and reusable idempotent browser mutations.

## Work scope

- app/auth/{router.py,service.py,session_service.py,user_repository.py,session_repository.py,creator_profile_repository.py,user_role_repository.py,models.py}
- app/core/{access_middleware.py,idempotency_service.py,idempotency_repository.py,audit_writer.py,audit_repository.py,request_context.py}
- app/main.py, tests/conftest.py, tests/e2e/test_auth.py

## Goal

Give the browser one HttpOnly session cookie while allowing immediate revocation after logout, user disablement, or role removal. Provide replay-safe mutations to every later module.

## Acceptance criteria

- Register/login return UserView and an fs_session cookie; neither raw password nor raw session token is persisted or logged. Password is 12–128 characters; creator handle is 3–32 lowercase letters, digits, or underscores.
- Logout, disabled user, expired/revoked session, and removed role immediately deny access.
- PATCH /v1/me/creator-profile atomically upserts the profile, grants creator, writes audit data, and rotates the current session.
- Same user/scope/idempotency key replays stored status/body/headers; same key with a different request hash returns 409 idempotency_conflict.
- Sessions persist only token SHA-256, user ID, expiry, revoked timestamp, created IP hash, user-agent hash, and optional rotated-from session ID. Browser JWT is never introduced.
- SameSite=Lax is required. Any approved cross-site mutation requires a CSRF token and trusted Origin validation; absent/invalid values are rejected before the service transaction.
- Register, login, logout, recipe save, listing publish, and payout paid/rejected each write their required request-correlated audit event in the same business transaction.

## Specification source

M1. Auth And Access Module; Session Mechanism; Why Not Browser JWT; Transaction And Consistency Policy (register/login/logout); Logging And Request Correlation Policy; Transport Rules; MVP Technical Limits; M1. Auth And Account.

## Dependencies

- T01: request context/logging; T02: users, roles, sessions, audit, idempotency tables.
- Modern password hashing, secrets, SHA-256, FastAPI cookie support.
- Use row locks plus unique (user_id, scope, idempotency_key); any Redis role cache is short-lived and invalidated on role change.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -c /tmp/creator.cookie -H 'Content-Type: application/json' -d '{"email":"creator@example.test","password":"correct-horse-01"}' http://localhost:8000/v1/auth/register
curl --noproxy '*' -sS -f -c /tmp/login.cookie -H 'Content-Type: application/json' -d '{"email":"creator@example.test","password":"correct-horse-01"}' http://localhost:8000/v1/auth/login
curl --noproxy '*' -sS -f -b /tmp/creator.cookie http://localhost:8000/v1/me
curl --noproxy '*' -sS -f -b /tmp/creator.cookie -c /tmp/creator.cookie -H 'Idempotency-Key: profile-0001' -H 'Content-Type: application/json' -d '{"handle":"creator_01","displayName":"Creator"}' http://localhost:8000/v1/me/creator-profile
curl --noproxy '*' -sS -o /tmp/csrf-error.json -w '%{http_code}\n' -b /tmp/creator.cookie -H 'Origin: https://cross-site.example' -H 'Idempotency-Key: profile-cross-site-0001' -H 'Content-Type: application/json' -d '{"handle":"creator_02","displayName":"Cross site"}' http://localhost:8000/v1/me/creator-profile
curl --noproxy '*' -sS -o /dev/null -w '%{http_code}\n' -b /tmp/creator.cookie -X POST http://localhost:8000/v1/auth/logout
curl --noproxy '*' -sS -o /dev/null -w '%{http_code}\n' -b /tmp/creator.cookie http://localhost:8000/v1/me
~~~

Expect register 201, login 200 with a rotated cookie, profile 200, cross-site mutation without the configured CSRF token 403, logout 204, then /me=401. Replay profile with identical key/body and expect saved 200; send another handle with that key and expect 409. Use the E2E disabled fixture from T00 to prove disabled login/mutation denial; test invalid password/handle bounds and an approved cross-site mutation with missing CSRF or Origin to prove rejection.

## Implementation plan

1. Validate email/password/handle limits and persist only a modern password hash.
2. Generate a random token, persist only SHA-256 plus created IP/user-agent hashes and rotation ancestry, and set HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=2592000.
3. Resolve active session, user status, and current roles in middleware; expose require_role and Origin/CSRF validation for approved cross-site mutations.
4. Rotate session on login, role change, and sensitive action; revoke and clear it on logout.
5. Implement idempotency claim/lease/request-hash/complete/replay; failed transactions are retryable, never cached as successful.
6. Expose a transaction-bound audit writer and require every named business transition to invoke it with no secret material.
