"""AI streaming state-machine, retry and lifetime-ledger contracts."""

from __future__ import annotations

import asyncio
import importlib
import json
from datetime import datetime, timezone
from types import SimpleNamespace
from uuid import UUID, uuid4

import pytest
from fastapi import HTTPException

import app.ai.service as ai_service
from app.ai.provider import ProviderUnavailable


def _reservation(
    *,
    retrying_partial: bool = False,
    replay: dict | None = None,
    history: list[dict] | None = None,
) -> ai_service.RequestReservation:
    return ai_service.RequestReservation(
        request_id=uuid4(),
        user_message_id=uuid4(),
        history=history or [],
        replay=replay,
        assistant_message_id=uuid4(),
        attempt_started_at=datetime.now(timezone.utc),
        retrying_partial=retrying_partial,
    )


def _decoded_events(chunks: list[bytes]) -> list[tuple[str, object]]:
    events: list[tuple[str, object]] = []
    for chunk in chunks:
        lines = chunk.decode().splitlines()
        event = next(line[7:] for line in lines if line.startswith("event: "))
        data = json.loads(next(line[6:] for line in lines if line.startswith("data: ")))
        events.append((event, data))
    return events


async def _collect(**kwargs) -> list[bytes]:
    return [chunk async for chunk in ai_service.stream_message(**kwargs)]


def _stream_arguments() -> dict[str, object]:
    return {
        "owner_id": uuid4(),
        "conversation_id": uuid4(),
        "idempotency_key": "retry-key",
        "user_text": "explain this",
        "context": {"spec": {"version": 1}, "member": False, "capabilities": {}},
        "image": None,
        "image_type": None,
        "force_patch": False,
    }


def _install_reservation(
    monkeypatch: pytest.MonkeyPatch,
    reservation: ai_service.RequestReservation,
) -> None:
    async def reserve_request(**_kwargs) -> ai_service.RequestReservation:
        return reservation

    async def refresh_lease(_request_id: UUID, _attempt_started_at: datetime) -> None:
        return None

    monkeypatch.setattr(ai_service, "reserve_request", reserve_request)
    monkeypatch.setattr(ai_service, "_refresh_lease", refresh_lease)


@pytest.mark.asyncio
async def test_failure_after_first_output_persists_billable_partial_and_partial_done(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    marked: list[UUID] = []
    persisted: list[dict[str, object]] = []
    message_id = uuid4()

    async def mark_started(request_id: UUID, _attempt_started_at: datetime) -> None:
        marked.append(request_id)

    async def persist_result(**kwargs) -> ai_service.PersistedResult:
        persisted.append(kwargs)
        return ai_service.PersistedResult(message_id, kwargs["final_status"])

    async def provider(**_kwargs):
        yield "delta", "partial output"
        raise ProviderUnavailable("upstream failed")

    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    monkeypatch.setattr(ai_service, "_persist_result", persist_result)
    monkeypatch.setattr(ai_service, "stream_completion", provider)
    arguments = _stream_arguments()

    events = _decoded_events(await _collect(**arguments))

    assert marked == [reservation.request_id]
    assert persisted == [
        {
            "request_id": reservation.request_id,
            "conversation_id": arguments["conversation_id"],
            "content": "partial output",
            "suggestion": None,
            "final_status": "partial",
            "assistant_message_id": reservation.assistant_message_id,
            "attempt_started_at": reservation.attempt_started_at,
        }
    ]
    assert events[-2:] == [
        ("error", {"code": "AI_PROVIDER_UNAVAILABLE", "messageId": str(message_id)}),
        ("done", {"messageId": str(message_id), "partial": True}),
    ]


@pytest.mark.asyncio
async def test_partial_retry_replaces_same_assistant_message_and_completes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation(
        retrying_partial=True,
        history=[{"role": "assistant", "content": "older unrelated answer"}],
    )
    _install_reservation(monkeypatch, reservation)
    persisted: list[dict[str, object]] = []
    provider_calls = 0

    async def mark_started(_request_id: UUID, _attempt_started_at: datetime) -> None:
        return None

    async def persist_result(**kwargs) -> ai_service.PersistedResult:
        persisted.append(kwargs)
        return ai_service.PersistedResult(
            reservation.assistant_message_id, kwargs["final_status"]
        )

    async def provider(**kwargs):
        nonlocal provider_calls
        provider_calls += 1
        assert kwargs["history"] == reservation.history
        yield "delta", "complete replacement"

    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    monkeypatch.setattr(ai_service, "_persist_result", persist_result)
    monkeypatch.setattr(ai_service, "stream_completion", provider)
    arguments = _stream_arguments()

    events = _decoded_events(await _collect(**arguments))

    assert provider_calls == 1
    assert len(persisted) == 1
    assert persisted[0]["assistant_message_id"] == reservation.assistant_message_id
    assert persisted[0]["final_status"] == "completed"
    assert persisted[0]["content"] == "complete replacement"
    assert events[-1] == ("done", {"messageId": str(reservation.assistant_message_id)})


@pytest.mark.asyncio
async def test_partial_retry_failure_before_new_output_restores_partial_without_new_charge(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation(retrying_partial=True)
    _install_reservation(monkeypatch, reservation)
    restored: list[tuple[UUID, datetime]] = []

    async def provider(**_kwargs):
        if False:
            yield "delta", "unreachable"
        raise ProviderUnavailable("rejected")

    async def settle_attempt(
        request_id: UUID, attempt_started_at: datetime
    ) -> ai_service.SettledAttempt:
        restored.append((request_id, attempt_started_at))
        return ai_service.SettledAttempt("partial", reservation.assistant_message_id)

    async def persist_result(**_kwargs) -> UUID:
        raise AssertionError("a retry with no new output must keep the prior partial message")

    monkeypatch.setattr(ai_service, "stream_completion", provider)
    monkeypatch.setattr(ai_service, "_settle_attempt", settle_attempt)
    monkeypatch.setattr(ai_service, "_persist_result", persist_result)
    arguments = _stream_arguments()

    events = _decoded_events(await _collect(**arguments))

    assert restored == [(reservation.request_id, reservation.attempt_started_at)]
    assert events[-2:] == [
        (
            "error",
            {
                "code": "AI_PROVIDER_UNAVAILABLE",
                "messageId": str(reservation.assistant_message_id),
            },
        ),
        ("done", {"messageId": str(reservation.assistant_message_id), "partial": True}),
    ]


@pytest.mark.asyncio
async def test_client_stop_after_delta_persists_partial_before_closing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    persisted: list[dict[str, object]] = []
    message_id = uuid4()

    async def mark_started(_request_id: UUID, _attempt_started_at: datetime) -> None:
        return None

    async def persist_result(**kwargs) -> ai_service.PersistedResult:
        persisted.append(kwargs)
        return ai_service.PersistedResult(message_id, kwargs["final_status"])

    async def provider(**_kwargs):
        yield "delta", "visible before stop"
        yield "delta", "must not be reached"

    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    monkeypatch.setattr(ai_service, "_persist_result", persist_result)
    monkeypatch.setattr(ai_service, "stream_completion", provider)
    arguments = _stream_arguments()
    iterator = ai_service.stream_message(**arguments)

    assert _decoded_events([await anext(iterator)])[0][0] == "message"
    assert _decoded_events([await anext(iterator)]) == [
        ("delta", {"content": "visible before stop"})
    ]
    await iterator.aclose()

    assert len(persisted) == 1
    assert persisted[0]["final_status"] == "partial"
    assert persisted[0]["content"] == "visible before stop"


@pytest.mark.asyncio
async def test_client_stop_before_provider_output_releases_unbilled_reservation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    released: list[tuple[UUID, datetime]] = []

    async def settle_attempt(
        request_id: UUID, attempt_started_at: datetime
    ) -> ai_service.SettledAttempt:
        released.append((request_id, attempt_started_at))
        return ai_service.SettledAttempt("released", None)

    async def provider(**_kwargs):
        yield "delta", "must not start"

    monkeypatch.setattr(ai_service, "_settle_attempt", settle_attempt)
    monkeypatch.setattr(ai_service, "stream_completion", provider)
    arguments = _stream_arguments()
    iterator = ai_service.stream_message(**arguments)

    assert _decoded_events([await anext(iterator)])[0][0] == "message"
    await iterator.aclose()

    assert released == [(reservation.request_id, reservation.attempt_started_at)]


@pytest.mark.asyncio
async def test_closing_after_done_does_not_downgrade_completed_request_to_partial(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    persisted_statuses: list[str] = []
    message_id = uuid4()

    async def mark_started(_request_id: UUID, _attempt_started_at: datetime) -> None:
        return None

    async def persist_result(**kwargs) -> ai_service.PersistedResult:
        persisted_statuses.append(kwargs["final_status"])
        return ai_service.PersistedResult(message_id, kwargs["final_status"])

    async def provider(**_kwargs):
        yield "delta", "complete"

    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    monkeypatch.setattr(ai_service, "_persist_result", persist_result)
    monkeypatch.setattr(ai_service, "stream_completion", provider)
    iterator = ai_service.stream_message(**_stream_arguments())

    assert _decoded_events([await anext(iterator)])[0][0] == "message"
    assert _decoded_events([await anext(iterator)])[0][0] == "delta"
    assert _decoded_events([await anext(iterator)]) == [
        ("done", {"messageId": str(message_id)})
    ]
    await iterator.aclose()

    assert persisted_statuses == ["completed"]


@pytest.mark.asyncio
async def test_completed_idempotent_request_only_replays_without_provider(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    message_id = uuid4()
    reservation = _reservation(
        replay={
            "id": message_id,
            "content": "saved answer",
            "suggestion": {"patch": {"iterations": 512}, "reason": "saved"},
        }
    )
    _install_reservation(monkeypatch, reservation)

    async def provider(**_kwargs):
        raise AssertionError("completed requests must never call the provider")
        yield "delta", "unreachable"

    monkeypatch.setattr(ai_service, "stream_completion", provider)
    arguments = _stream_arguments()

    events = _decoded_events(await _collect(**arguments))

    assert events == [
        ("message", {"id": str(message_id), "role": "assistant", "replayed": True}),
        ("delta", {"content": "saved answer"}),
        ("suggestion", reservation.replay["suggestion"]),
        ("done", {"messageId": str(message_id), "replayed": True}),
    ]


class _Result:
    def __init__(self, *, row=None, rows=None, rowcount: int = 0) -> None:
        self.row = row
        self.rows = rows or []
        self.rowcount = rowcount

    def mappings(self):
        return self

    def one_or_none(self):
        return self.row

    def one(self):
        assert self.row is not None
        return self.row

    def scalar_one_or_none(self):
        if isinstance(self.row, dict):
            return next(iter(self.row.values()))
        return self.row

    def all(self):
        return self.rows


class _TransactionContext:
    def __init__(self, connection) -> None:
        self.connection = connection

    async def __aenter__(self):
        return self.connection

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        return None


class _ReservationConnection:
    def __init__(self, *, owner_id: UUID, conversation_id: UUID, prior: dict | None,
                 history: list[dict] | None = None, member: bool = False,
                 active: int = 0, used: int = 0, pending: int = 0,
                 conversation_exists: bool = True) -> None:
        self.owner_id = owner_id
        self.conversation_id = conversation_id
        self.prior = prior
        self.history = history or []
        self.member = member
        self.active = active
        self.trial_used = used + pending
        self.conversation_exists = conversation_exists
        self.statements: list[tuple[str, dict | None]] = []

    async def execute(self, statement, parameters=None):
        query = " ".join(str(statement).split())
        self.statements.append((query, parameters))
        if query.startswith("SELECT id, title, optimization_consent"):
            return _Result(row={"id": self.conversation_id} if self.conversation_exists else None)
        if "FROM ai_requests r" in query:
            return _Result(row=self.prior)
        if query.startswith("SELECT count(*) FILTER"):
            return _Result(row={"active": self.active, "trial_used": self.trial_used})
        if query.startswith("SELECT role, content FROM ai_messages"):
            return _Result(rows=self.history)
        if "RETURNING id" in query:
            return _Result(row=uuid4(), rowcount=1)
        return _Result()

    async def scalar(self, statement, parameters=None):
        query = " ".join(str(statement).split())
        self.statements.append((query, parameters))
        if "FROM memberships" in query:
            return 1 if self.member else None
        raise AssertionError(f"unexpected scalar query: {query}")


class _Engine:
    def __init__(self, connection) -> None:
        self.connection = connection

    def begin(self) -> _TransactionContext:
        return _TransactionContext(self.connection)

    def connect(self) -> _TransactionContext:
        return _TransactionContext(self.connection)


def _settings() -> SimpleNamespace:
    return SimpleNamespace(
        ai_max_concurrent_per_user=2,
        ai_free_lifetime_limit=10,
        ai_history_ttl_days=90,
    )


def _prior(*, conversation_id: UUID, request_hash: str, status: str,
           user_message_id: UUID, assistant_message_id: UUID | None = None) -> dict:
    return {
        "request_id": uuid4(),
        "status": status,
        "conversation_id": conversation_id,
        "user_message_id": user_message_id,
        "request_hash": request_hash,
        "first_output_at": object() if status == "partial" else None,
        "assistant_message_id": assistant_message_id,
        "persisted_assistant_id": assistant_message_id,
        "counts_toward_trial": True,
        "content": "old partial" if assistant_message_id else None,
        "suggestion": None,
        "created_at": None,
    }


@pytest.mark.asyncio
async def test_partial_reservation_reuses_messages_and_excludes_them_from_retry_history(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, conversation_id = uuid4(), uuid4()
    user_message_id, assistant_message_id = uuid4(), uuid4()
    request_hash = "a" * 64
    connection = _ReservationConnection(
        owner_id=owner_id,
        conversation_id=conversation_id,
        prior=_prior(
            conversation_id=conversation_id,
            request_hash=request_hash,
            status="partial",
            user_message_id=user_message_id,
            assistant_message_id=assistant_message_id,
        ),
        history=[
            {"role": "assistant", "content": "older answer"},
            {"role": "user", "content": "older question"},
        ],
        used=10,
    )
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    reservation = await ai_service.reserve_request(
        owner_id=owner_id,
        conversation_id=conversation_id,
        idempotency_key="partial-retry",
        user_text="same question",
        request_hash=request_hash,
    )

    assert reservation.retrying_partial is True
    assert reservation.user_message_id == user_message_id
    assert reservation.assistant_message_id == assistant_message_id
    assert reservation.history == [
        {"role": "user", "content": "older question"},
        {"role": "assistant", "content": "older answer"},
    ]
    queries = [query for query, _parameters in connection.statements]
    assert "pg_advisory_xact_lock" in queries[0]
    assert not any("INSERT INTO ai_messages" in query for query in queries)
    assert any("SET status='retrying'" in query for query in queries)
    history_query, history_parameters = next(
        (query, parameters)
        for query, parameters in connection.statements
        if query.startswith("SELECT role, content FROM ai_messages")
    )
    assert "id<>:user_message_id" in history_query
    assert "id<>:assistant_message_id" in history_query
    assert history_parameters["user_message_id"] == user_message_id
    assert history_parameters["assistant_message_id"] == assistant_message_id
    quota_queries = [query for query in queries if query.startswith("SELECT count(*) FILTER")]
    assert len(quota_queries) == 1
    assert "counts_toward_trial" in quota_queries[0]
    assert "'reserved','streaming','retrying','partial','completed'" in quota_queries[0]


@pytest.mark.asyncio
async def test_released_retry_reuses_user_message_without_inserting_duplicate(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, conversation_id, user_message_id = uuid4(), uuid4(), uuid4()
    request_hash = "b" * 64
    connection = _ReservationConnection(
        owner_id=owner_id,
        conversation_id=conversation_id,
        prior=_prior(
            conversation_id=conversation_id,
            request_hash=request_hash,
            status="released",
            user_message_id=user_message_id,
        ),
    )
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    reservation = await ai_service.reserve_request(
        owner_id=owner_id,
        conversation_id=conversation_id,
        idempotency_key="released-retry",
        user_text="same question",
        request_hash=request_hash,
    )

    queries = [query for query, _parameters in connection.statements]
    assert reservation.user_message_id == user_message_id
    assert reservation.retrying_partial is False
    assert not any("INSERT INTO ai_messages" in query for query in queries)
    assert any("SET status='reserved'" in query for query in queries)
    retry_parameters = next(
        parameters for query, parameters in connection.statements
        if "SET status='reserved'" in query
    )
    assert retry_parameters["counts"] is True


@pytest.mark.asyncio
async def test_quota_admission_counts_billable_and_pending_requests_under_owner_lock(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, conversation_id = uuid4(), uuid4()
    connection = _ReservationConnection(
        owner_id=owner_id,
        conversation_id=conversation_id,
        prior=None,
        used=9,
        pending=1,
    )
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    with pytest.raises(HTTPException) as raised:
        await ai_service.reserve_request(
            owner_id=owner_id,
            conversation_id=conversation_id,
            idempotency_key="at-limit",
            user_text="new question",
            request_hash="c" * 64,
        )

    assert raised.value.status_code == 402
    assert raised.value.detail == "AI_TRIAL_EXHAUSTED"
    queries = [query for query, _parameters in connection.statements]
    assert "pg_advisory_xact_lock" in queries[0]
    quota_queries = [query for query in queries if query.startswith("SELECT count(*) FILTER")]
    assert len(quota_queries) == 1
    assert "counts_toward_trial" in quota_queries[0]
    assert "'reserved','streaming','retrying','partial','completed'" in quota_queries[0]
    assert not any("INSERT INTO ai_requests" in query for query in queries)


@pytest.mark.asyncio
async def test_deleted_conversation_cannot_replay_or_move_orphaned_ledger_key(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, new_conversation_id, user_message_id = uuid4(), uuid4(), uuid4()
    request_hash = "d" * 64
    orphaned = _prior(
        conversation_id=new_conversation_id,
        request_hash=request_hash,
        status="completed",
        user_message_id=user_message_id,
        assistant_message_id=None,
    )
    orphaned["conversation_id"] = None
    connection = _ReservationConnection(
        owner_id=owner_id,
        conversation_id=new_conversation_id,
        prior=orphaned,
    )
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    with pytest.raises(HTTPException) as raised:
        await ai_service.reserve_request(
            owner_id=owner_id,
            conversation_id=new_conversation_id,
            idempotency_key="orphaned-key",
            user_text="same question",
            request_hash=request_hash,
        )

    assert raised.value.status_code == 409
    assert raised.value.detail == "idempotency_conflict"


@pytest.mark.asyncio
async def test_persisted_partial_retry_updates_original_assistant_row(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, conversation_id, request_id = uuid4(), uuid4(), uuid4()
    assistant_message_id = uuid4()
    attempt_started_at = datetime.now(timezone.utc)

    class PersistenceConnection:
        def __init__(self) -> None:
            self.statements: list[tuple[str, dict | None]] = []

        async def scalar(self, statement, parameters=None):
            query = " ".join(str(statement).split())
            self.statements.append((query, parameters))
            if query.startswith("SELECT owner_id FROM ai_requests"):
                return owner_id
            if query.startswith("SELECT 1 FROM ai_conversations"):
                return 1
            raise AssertionError(query)

        async def execute(self, statement, parameters=None):
            query = " ".join(str(statement).split())
            self.statements.append((query, parameters))
            if query.startswith("SELECT status,conversation_id"):
                return _Result(row={
                    "status": "retrying",
                    "conversation_id": conversation_id,
                    "assistant_message_id": assistant_message_id,
                    "attempt_started_at": attempt_started_at,
                })
            if "RETURNING id" in query:
                return _Result(row=assistant_message_id, rowcount=1)
            return _Result()

    connection = PersistenceConnection()
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))

    result = await ai_service._persist_result(
        request_id=request_id,
        conversation_id=conversation_id,
        content="replacement",
        suggestion=None,
        final_status="completed",
        assistant_message_id=assistant_message_id,
        attempt_started_at=attempt_started_at,
    )

    assert result == ai_service.PersistedResult(assistant_message_id, "completed")
    queries = [query for query, _parameters in connection.statements]
    assert any("DELETE FROM ai_feedback" in query for query in queries)
    assert any("INSERT INTO ai_messages" in query and "ON CONFLICT(id)" in query for query in queries)
    request_update = next(
        parameters
        for query, parameters in connection.statements
        if query.startswith("UPDATE ai_requests SET status=:status")
    )
    assert request_update["status"] == "completed"
    assert request_update["mid"] == assistant_message_id
    assert request_update["attempt"] == attempt_started_at


class _LedgerConnection:
    def __init__(self, conversation_id: UUID) -> None:
        self.requests = [
            {"conversation_id": conversation_id, "status": "completed", "counts": True},
            {"conversation_id": conversation_id, "status": "partial", "counts": True},
            {"conversation_id": conversation_id, "status": "retrying", "counts": True},
            {"conversation_id": conversation_id, "status": "streaming", "counts": True},
            {"conversation_id": conversation_id, "status": "released", "counts": True},
            {"conversation_id": conversation_id, "status": "completed", "counts": False},
        ]
        self.queries: list[str] = []

    async def scalar(self, statement, _parameters=None):
        query = " ".join(str(statement).split())
        self.queries.append(query)
        if "FROM memberships" in query:
            return None
        if "FROM ai_requests" in query:
            billable = {"reserved", "completed", "partial", "retrying", "streaming"}
            return sum(
                request["counts"] and request["status"] in billable
                for request in self.requests
            )
        raise AssertionError(f"unexpected query: {query}")

    def delete_conversation(self, conversation_id: UUID) -> None:
        for request in self.requests:
            if request["conversation_id"] == conversation_id:
                request["conversation_id"] = None


@pytest.mark.asyncio
async def test_deleting_conversation_does_not_restore_lifetime_allowance(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    conversation_id = uuid4()
    ledger = _LedgerConnection(conversation_id)
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(ledger))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    before = await ai_service.allowance(uuid4())
    ledger.delete_conversation(conversation_id)
    after = await ai_service.allowance(uuid4())

    assert before == after == {"member": False, "limit": 10, "used": 4, "remaining": 6}
    assert all(request["conversation_id"] is None for request in ledger.requests)
    assert all(
        "status IN ('reserved','streaming','retrying','partial','completed')" in query
        and "counts_toward_trial" in query
        for query in ledger.queries
        if "FROM ai_requests" in query
    )


def test_partial_retry_migration_extends_statuses_and_preserves_ledger_on_delete(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    migration = importlib.import_module(
        "migrations.versions.20260809_0020_ai_partial_retry_ledger"
    )
    statements: list[str] = []
    monkeypatch.setattr(migration.op, "execute", statements.append)

    migration.upgrade()

    sql_text = "\n".join(statements)
    assert "'retrying','partial'" in sql_text
    assert "status='partial'" in sql_text
    assert "ALTER COLUMN conversation_id DROP NOT NULL" in sql_text
    assert "ON DELETE SET NULL" in sql_text
    assert "counts_toward_trial" in sql_text
    assert "attempt_started_at" in sql_text
    assert "lease_until" in sql_text
    assert "assistant_message_id_fkey" in sql_text
    assert "idempotency_key DROP NOT NULL" in sql_text
    assert "WHERE idempotency_key IS NOT NULL" in sql_text
    assert "DELETE FROM ai_requests" not in sql_text

    with pytest.raises(RuntimeError, match="cannot be downgraded"):
        migration.downgrade()


@pytest.mark.asyncio
async def test_missing_tool_response_retries_once_before_failing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """DeepSeek 'auto' tool choice can return text only; the first attempt must
    retry because phase one regenerates a different observation."""

    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    calls: list[int] = []
    settled: list[tuple[UUID, datetime]] = []

    async def provider(**_kwargs) -> object:
        calls.append(1)
        return
        yield  # pragma: no cover - makes this an async generator

    async def settle(
        request_id: UUID, attempt_started_at: datetime
    ) -> ai_service.SettledAttempt:
        settled.append((request_id, attempt_started_at))
        return ai_service.SettledAttempt("released", None)

    monkeypatch.setattr(ai_service, "stream_completion", provider)
    monkeypatch.setattr(ai_service, "_settle_attempt", settle)
    arguments = _stream_arguments()
    arguments["assistant_mode"] = "color"

    events = _decoded_events(await _collect(**arguments))

    assert len(calls) == 2
    assert events[-1] == ("error", {"code": "AI_PROVIDER_UNAVAILABLE"})


@pytest.mark.asyncio
async def test_unexpected_runtime_error_before_output_releases_attempt(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    settled: list[tuple[UUID, datetime]] = []

    async def provider(**_kwargs):
        if False:
            yield "delta", "unreachable"
        raise RuntimeError("unexpected adapter bug")

    async def settle(
        request_id: UUID, attempt_started_at: datetime
    ) -> ai_service.SettledAttempt:
        settled.append((request_id, attempt_started_at))
        return ai_service.SettledAttempt("released", None)

    monkeypatch.setattr(ai_service, "stream_completion", provider)
    monkeypatch.setattr(ai_service, "_settle_attempt", settle)

    events = _decoded_events(await _collect(**_stream_arguments()))

    assert settled == [(reservation.request_id, reservation.attempt_started_at)]
    assert events[-1] == ("error", {"code": "AI_PROVIDER_UNAVAILABLE"})


@pytest.mark.asyncio
async def test_cancellation_after_output_terminalizes_partial_before_propagating(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    persisted: list[dict[str, object]] = []

    async def mark_started(_request_id: UUID, _attempt: datetime) -> None:
        return None

    async def persist(**kwargs) -> ai_service.PersistedResult:
        persisted.append(kwargs)
        return ai_service.PersistedResult(
            reservation.assistant_message_id, kwargs["final_status"]
        )

    async def provider(**_kwargs):
        yield "delta", "visible"
        raise asyncio.CancelledError()

    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    monkeypatch.setattr(ai_service, "_persist_result", persist)
    monkeypatch.setattr(ai_service, "stream_completion", provider)

    with pytest.raises(asyncio.CancelledError):
        await _collect(**_stream_arguments())

    assert [call["final_status"] for call in persisted] == ["partial"]
    assert persisted[0]["attempt_started_at"] == reservation.attempt_started_at


@pytest.mark.asyncio
async def test_partial_retry_keeps_acceptance_charge_flag_after_membership_expires(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, conversation_id = uuid4(), uuid4()
    request_hash = "e" * 64
    prior = _prior(
        conversation_id=conversation_id,
        request_hash=request_hash,
        status="partial",
        user_message_id=uuid4(),
        assistant_message_id=uuid4(),
    )
    prior["counts_toward_trial"] = False
    connection = _ReservationConnection(
        owner_id=owner_id,
        conversation_id=conversation_id,
        prior=prior,
        member=False,
        used=10,
    )
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    reservation = await ai_service.reserve_request(
        owner_id=owner_id,
        conversation_id=conversation_id,
        idempotency_key="member-partial-retry",
        user_text="same",
        request_hash=request_hash,
    )

    assert reservation.retrying_partial is True
    retry_parameters = next(
        parameters for query, parameters in connection.statements
        if "SET status='retrying'" in query
    )
    assert "counts" not in retry_parameters


@pytest.mark.asyncio
async def test_released_retry_recomputes_charge_flag_after_membership_expires(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, conversation_id = uuid4(), uuid4()
    request_hash = "f" * 64
    prior = _prior(
        conversation_id=conversation_id,
        request_hash=request_hash,
        status="released",
        user_message_id=uuid4(),
    )
    prior["counts_toward_trial"] = False
    connection = _ReservationConnection(
        owner_id=owner_id,
        conversation_id=conversation_id,
        prior=prior,
        member=False,
        used=9,
    )
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(ai_service, "get_settings", _settings)

    await ai_service.reserve_request(
        owner_id=owner_id,
        conversation_id=conversation_id,
        idempotency_key="released-after-expiry",
        user_text="same",
        request_hash=request_hash,
    )

    retry_parameters = next(
        parameters for query, parameters in connection.statements
        if "SET status='reserved'" in query
    )
    assert retry_parameters["counts"] is True


@pytest.mark.asyncio
async def test_ambiguous_partial_persist_observes_completed_without_downgrade(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, request_id, conversation_id = uuid4(), uuid4(), uuid4()
    message_id = uuid4()
    attempt = datetime.now(timezone.utc)

    class CompletedConnection:
        def __init__(self) -> None:
            self.queries: list[str] = []

        async def scalar(self, statement, _parameters=None):
            query = " ".join(str(statement).split())
            self.queries.append(query)
            assert query.startswith("SELECT owner_id FROM ai_requests")
            return owner_id

        async def execute(self, statement, _parameters=None):
            query = " ".join(str(statement).split())
            self.queries.append(query)
            if query.startswith("SELECT status,conversation_id"):
                return _Result(row={
                    "status": "completed",
                    "conversation_id": conversation_id,
                    "assistant_message_id": message_id,
                    "attempt_started_at": attempt,
                })
            return _Result()

    connection = CompletedConnection()
    monkeypatch.setattr(ai_service, "get_engine", lambda: _Engine(connection))

    result = await ai_service._persist_result(
        request_id=request_id,
        conversation_id=conversation_id,
        content="late partial",
        suggestion=None,
        final_status="partial",
        assistant_message_id=message_id,
        attempt_started_at=attempt,
    )

    assert result == ai_service.PersistedResult(message_id, "completed")
    assert not any("DELETE FROM ai_feedback" in query for query in connection.queries)
    assert not any("SET status=:status" in query for query in connection.queries)


@pytest.mark.asyncio
async def test_expired_lease_recovery_uses_first_output_to_choose_terminal_state() -> None:
    class RecoveryConnection:
        def __init__(self) -> None:
            self.query = ""
            self.parameters = None

        async def execute(self, statement, parameters=None):
            self.query = " ".join(str(statement).split())
            self.parameters = parameters
            return _Result(rowcount=2)

    owner_id = uuid4()
    connection = RecoveryConnection()

    recovered = await ai_service.recover_expired_requests(
        connection, owner_id=owner_id
    )

    assert recovered == 2
    assert "first_output_at IS NULL THEN 'released' ELSE 'partial'" in connection.query
    assert "lease_until IS NULL OR lease_until<=now()" in connection.query
    assert "status IN ('reserved','streaming','retrying')" in connection.query
    assert connection.parameters == {"owner": owner_id}


@pytest.mark.asyncio
async def test_two_phase_image_flow_uploads_image_only_for_visual_observation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    provider_images: list[tuple[bytes | None, str | None]] = []
    marks: list[UUID] = []
    persisted: list[dict[str, object]] = []
    suggestion = {"candidates": [{"patch": {"colorMap": "viridis"}}]}

    async def provider(**kwargs):
        provider_images.append((kwargs["image"], kwargs["image_type"]))
        if len(provider_images) == 1:
            assert "相对位置、画面裁切和留白" in kwargs["text"]
            yield "delta", "第一次观察不应显示。"
        elif len(provider_images) == 2:
            raise ProviderUnavailable("tool connection timed out", retryable=True)
        elif len(provider_images) == 3:
            yield "delta", "主体居中，四周留白较少。"
        else:
            yield "delta", "工具调用前的文字也不应提前显示。"
            yield "suggestion", suggestion

    async def mark_started(request_id: UUID, _attempt: datetime) -> None:
        marks.append(request_id)

    async def persist(**kwargs) -> ai_service.PersistedResult:
        persisted.append(kwargs)
        return ai_service.PersistedResult(
            reservation.assistant_message_id, kwargs["final_status"]
        )

    monkeypatch.setattr(ai_service, "stream_completion", provider)
    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    monkeypatch.setattr(ai_service, "_persist_result", persist)
    monkeypatch.setattr(ai_service, "validate_candidate_set", lambda payload, *_args: payload)
    arguments = _stream_arguments()
    arguments.update({
        "image": b"preview-bytes",
        "image_type": "image/webp",
        "assistant_mode": "color",
    })

    events = _decoded_events(await _collect(**arguments))

    assert provider_images == [
        (b"preview-bytes", "image/webp"),
        (None, None),
        (b"preview-bytes", "image/webp"),
        (None, None),
    ]
    assert marks == [reservation.request_id]
    assert [payload for event, payload in events if event == "delta"] == [
        {"content": "主体居中，四周留白较少。"}
    ]
    assert any(event == "suggestion" and payload == suggestion for event, payload in events)
    assert persisted[0]["content"] == "主体居中，四周留白较少。"


@pytest.mark.asyncio
async def test_invalid_two_phase_tool_result_is_not_retried_or_billed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reservation = _reservation()
    _install_reservation(monkeypatch, reservation)
    provider_calls = 0
    settled: list[UUID] = []

    async def provider(**kwargs):
        nonlocal provider_calls
        provider_calls += 1
        if kwargs["image"] is not None:
            yield "delta", "暂存的观察"
        else:
            yield "suggestion", {"invalid": True}

    async def settle(
        request_id: UUID, _attempt: datetime
    ) -> ai_service.SettledAttempt:
        settled.append(request_id)
        return ai_service.SettledAttempt("released", None)

    async def mark_started(*_args) -> None:
        raise AssertionError("invalid tool output must not start billing")

    monkeypatch.setattr(ai_service, "stream_completion", provider)
    monkeypatch.setattr(ai_service, "validate_candidate_set", lambda *_args: None)
    monkeypatch.setattr(ai_service, "_settle_attempt", settle)
    monkeypatch.setattr(ai_service, "_mark_started", mark_started)
    arguments = _stream_arguments()
    arguments.update({
        "image": b"preview-bytes",
        "image_type": "image/webp",
        "assistant_mode": "composition",
    })

    events = _decoded_events(await _collect(**arguments))

    assert provider_calls == 2
    assert settled == [reservation.request_id]
    assert not any(event in {"delta", "suggestion"} for event, _payload in events)
    assert events[-1] == ("error", {"code": "AI_PROVIDER_UNAVAILABLE"})
