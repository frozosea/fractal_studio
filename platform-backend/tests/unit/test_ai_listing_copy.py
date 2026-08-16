"""Deterministic boundaries for AI listing copy; model quality is a real contract test."""

from __future__ import annotations

import importlib
import io
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace
from uuid import uuid4

import pytest
from fastapi import HTTPException
from PIL import Image
from pydantic import SecretStr, ValidationError

import app.ai.listing_service as listing_service
import app.ai.listing_provider as listing_provider
from app.ai.listing_models import (
    FocusedListingCopyToolResult,
    ListingCopyInput,
    ListingCopyToolResult,
)
from app.ai.listing_provider import (
    ListingProviderUnavailable,
    _parse_completion,
    _without_unverified_identities,
)
from app.infrastructure.storage.object_storage import ObjectStorage


def _candidate(number: int) -> dict[str, object]:
    return {
        "title": f"潮汐纹理 {number}",
        "description": f"深蓝结构向暖色边缘展开，形成第 {number} 种清晰的视觉重心。",
        "tags": ["分形", f"潮汐{number}", "深蓝"],
    }


def _candidates() -> list[dict[str, object]]:
    return [_candidate(number) for number in range(1, 4)]


def test_request_contract_distinguishes_initial_generation_and_revision() -> None:
    listing_id, source_id = uuid4(), uuid4()
    initial = ListingCopyInput.model_validate({"listingId": listing_id, "locale": "zh"})
    revision = ListingCopyInput.model_validate(
        {
            "listingId": listing_id,
            "locale": "en",
            "sourceRequestId": source_id,
            "instruction": "  use a quieter title  ",
        }
    )

    assert initial.source_request_id is None
    assert revision.instruction == "use a quieter title"
    with pytest.raises(ValidationError):
        ListingCopyInput.model_validate(
            {"listingId": listing_id, "locale": "zh", "sourceRequestId": source_id}
        )
    with pytest.raises(ValidationError):
        ListingCopyInput.model_validate(
            {"listingId": listing_id, "locale": "zh", "instruction": "revise"}
        )
    with pytest.raises(ValidationError):
        ListingCopyInput.model_validate(
            {"listingId": listing_id, "locale": "fr", "unexpected": True}
        )


def test_provider_candidates_are_exactly_three_unique_and_listing_compatible() -> None:
    raw = _candidates()
    raw[0]["tags"] = ["#Fractal", " Deep Blue ", "Ocean"]
    checked = ListingCopyToolResult.model_validate({"candidates": raw})

    assert checked.candidates[0].tags == ["fractal", "deep blue", "ocean"]
    duplicate = _candidates()
    duplicate[2] = duplicate[0]
    with pytest.raises(ValidationError):
        ListingCopyToolResult.model_validate({"candidates": duplicate})
    with pytest.raises(ValidationError):
        ListingCopyToolResult.model_validate({"candidates": _candidates()[:2]})
    too_long = _candidates()
    too_long[0]["title"] = "x" * 121
    with pytest.raises(ValidationError):
        ListingCopyToolResult.model_validate({"candidates": too_long})


@pytest.mark.asyncio
async def test_listing_provider_reveals_secret_only_at_each_outbound_call(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    sentinel = "sf-listing-secret-never-render"
    outbound_keys: list[str] = []

    async def post_completion(_client, *, payload, api_key):
        del payload
        outbound_keys.append(api_key)
        if len(outbound_keys) == 1:
            return {"choices": [{"message": {"content": "可信的纯视觉观察"}}]}
        raise RuntimeError("stop after checking the second outbound key")

    monkeypatch.setattr(listing_provider, "_post_completion", post_completion)
    settings = SimpleNamespace(
        siliconflow_api_key=SecretStr(sentinel),
        siliconflow_base_url="https://provider.invalid/v1",
        siliconflow_model="test/model",
        ai_max_output_tokens=512,
    )

    with pytest.raises(RuntimeError, match="second outbound key"):
        await listing_provider.generate_listing_copy(
            locale="zh",
            listing_context={},
            image=b"preview",
            image_type="image/png",
            settings=settings,  # type: ignore[arg-type]
        )

    assert outbound_keys == [sentinel, sentinel]
    assert sentinel not in repr(settings)


def test_preview_is_resized_and_kept_below_hard_in_memory_limit() -> None:
    image = Image.effect_noise((1600, 900), 80).convert("RGB")
    source = io.BytesIO()
    image.save(source, "PNG")

    encoded, media_type = listing_service.prepare_listing_preview(source.getvalue())

    assert media_type == "image/jpeg"
    assert len(encoded) <= 1_048_576
    with Image.open(io.BytesIO(encoded)) as preview:
        assert max(preview.size) == 640


def test_preview_rejects_decompression_sized_images_before_loading(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class OversizedImage:
        width = 5000
        height = 5000

        def __enter__(self):
            return self

        def __exit__(self, *_args) -> None:
            return None

        def load(self) -> None:
            raise AssertionError("oversized image must be rejected before pixel decoding")

    monkeypatch.setattr(listing_service.Image, "open", lambda *_args, **_kwargs: OversizedImage())

    with pytest.raises(listing_service.ListingPreviewUnavailable):
        listing_service.prepare_listing_preview(b"header-only")


@pytest.mark.asyncio
async def test_private_preview_download_is_bounded_and_closes_stream(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class Body:
        closed = False

        def read(self, amount: int) -> bytes:
            assert amount == 6
            return b"image"

        def close(self) -> None:
            self.closed = True

    body = Body()

    class Client:
        def get_object(self, **kwargs):
            assert kwargs == {"Bucket": "private-test", "Key": "asset/thumbnail.png"}
            return {"Body": body, "ContentLength": 5}

    storage = ObjectStorage(SimpleNamespace(s3_bucket="private-test"))  # type: ignore[arg-type]
    monkeypatch.setattr(storage, "_client", Client)

    assert await storage.download_bytes(
        object_key="asset/thumbnail.png", max_bytes=5
    ) == b"image"
    assert body.closed is True


def test_provider_response_parser_rejects_text_or_invalid_tool_candidates() -> None:
    focused = [
        {**candidate, "focus": focus, "description": description}
        for candidate, focus, description in zip(
            _candidates(),
            ("composition", "color", "texture"),
            (
                "两片黑色负空间分居上方与左下，亮色结构沿对角线连接画面。",
                "深紫色在外围过渡到橙黄与乳白，高亮区域分布在左上和右下。",
                "平滑的宽色带贴近细密颗粒边缘，形成连续而有节奏的纹理对比。",
            ),
            strict=True,
        )
    ]
    valid = {
        "choices": [
            {
                "message": {
                    "tool_calls": [
                        {
                            "function": {
                                "name": "propose_listing_copy",
                                "arguments": __import__("json").dumps(
                                    {"candidates": focused}, ensure_ascii=False
                                ),
                            }
                        }
                    ]
                }
            }
        ],
        "usage": {"total_tokens": 100},
    }

    assert len(_parse_completion(valid).candidates) == 3
    with pytest.raises(ListingProviderUnavailable):
        _parse_completion({"choices": [{"message": {"content": "plain text"}}]})
    valid["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"] = "{}"
    with pytest.raises(ListingProviderUnavailable):
        _parse_completion(valid)


def test_provider_focus_contract_rejects_repeated_descriptions() -> None:
    focused = [
        {
            **candidate,
            "focus": focus,
            "description": "紫色与橙色形成渐变，细密边缘与黑色背景形成明显对比。",
        }
        for candidate, focus in zip(
            _candidates(),
            ("composition", "color", "texture"),
            strict=True,
        )
    ]

    with pytest.raises(ValidationError):
        FocusedListingCopyToolResult.model_validate({"candidates": focused})


def test_specific_unverified_math_identity_is_removed_from_tags_and_rejected_in_copy() -> None:
    candidates = ListingCopyToolResult.model_validate({"candidates": _candidates()}).candidates
    candidates[0] = candidates[0].model_copy(
        update={"tags": [*candidates[0].tags, "曼德博集合"]}
    )

    sanitized = _without_unverified_identities(candidates)

    assert "曼德博集合" not in sanitized[0].tags
    candidates[1] = candidates[1].model_copy(update={"title": "Mandelbrot boundary"})
    with pytest.raises(ListingProviderUnavailable):
        _without_unverified_identities(candidates)


def test_model_context_grounds_copy_without_leaking_coordinates_or_formula_source() -> None:
    source = listing_service.ListingSource(
        listing_id=uuid4(),
        asset_id=uuid4(),
        status="draft",
        title="Current",
        description="Current description",
        tags=["existing"],
        canonical_spec={
            "version": 1,
            "centerRe": "-0.743643887",
            "centerIm": "0.131825904",
            "variant": "mandelbrot",
            "iterations": 1024,
            "orbitProgram": {"private": "formula source"},
            "colorProgram": {"stops": [{"at": 0}, {"at": 1}]},
        },
        output_spec={"width": 2400, "height": 1600},
        preview_object_key="private/asset/thumbnail.png",
    )

    context = listing_service._listing_context(source)
    rendered = str(context)

    assert "centerRe" not in rendered
    assert "formula source" not in rendered
    assert context["render"]["formulaKind"] == "custom"
    assert context["render"]["colorProgram"] == {
        "kind": "customGradient",
        "stopCount": 2,
    }


class _Result:
    def __init__(self, *, row=None, rowcount: int = 0) -> None:
        self.row = row
        self.rowcount = rowcount

    def mappings(self):
        return self

    def one_or_none(self):
        return self.row

    def one(self):
        return self.row

    def scalar_one_or_none(self):
        return self.row


class _Context:
    def __init__(self, connection) -> None:
        self.connection = connection

    async def __aenter__(self):
        return self.connection

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        return None


class _Engine:
    def __init__(self, connection) -> None:
        self.connection = connection

    def begin(self) -> _Context:
        return _Context(self.connection)

    def connect(self) -> _Context:
        return _Context(self.connection)


class _LedgerConnection:
    def __init__(self, *, prior=None, member=False, active=0, used=0, pending=0) -> None:
        self.prior = prior
        self.member = member
        self.active = active
        self.used = used
        self.pending = pending
        self.statements: list[str] = []
        self.parameters: list[dict[str, object] | None] = []

    async def execute(self, statement, parameters=None):
        query = " ".join(str(statement).split())
        self.statements.append(query)
        self.parameters.append(parameters)
        if "FROM ai_requests r" in query:
            return _Result(row=self.prior)
        if "count(*) FILTER" in query:
            return _Result(row={"active": self.active, "trial_used": self.used + self.pending})
        return _Result()

    async def scalar(self, statement, parameters=None):
        query = " ".join(str(statement).split())
        self.statements.append(query)
        self.parameters.append(parameters)
        if "FROM memberships" in query:
            return 1 if self.member else None
        raise AssertionError(f"unexpected scalar query: {query}")


def _settings() -> SimpleNamespace:
    return SimpleNamespace(
        ai_max_concurrent_per_user=2,
        ai_free_lifetime_limit=10,
        ai_history_ttl_days=90,
        ai_enabled=True,
    )


@pytest.mark.asyncio
async def test_listing_copy_reservation_uses_same_billable_and_pending_lifetime_quota(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    connection = _LedgerConnection(used=9, pending=1)
    monkeypatch.setattr(listing_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(listing_service, "get_settings", _settings)

    with pytest.raises(HTTPException) as raised:
        await listing_service.reserve_listing_copy(
            owner_id=uuid4(),
            idempotency_key="listing-copy-limit",
            request_hash="a" * 64,
        )

    assert raised.value.status_code == 402
    assert raised.value.detail == "AI_TRIAL_EXHAUSTED"
    assert "pg_advisory_xact_lock" in connection.statements[0]
    assert any("counts_toward_trial" in query for query in connection.statements)
    assert not any("INSERT INTO ai_requests" in query for query in connection.statements)


@pytest.mark.asyncio
async def test_completed_listing_copy_idempotency_replays_saved_candidates_without_new_quota(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    request_id = uuid4()
    prior = {
        "request_id": request_id,
        "status": "completed",
        "request_hash": "b" * 64,
        "candidates": _candidates(),
        "expires_at": datetime.now(UTC) + timedelta(days=1),
    }
    connection = _LedgerConnection(prior=prior, used=10, pending=0)
    monkeypatch.setattr(listing_service, "get_engine", lambda: _Engine(connection))
    monkeypatch.setattr(listing_service, "get_settings", _settings)

    reservation = await listing_service.reserve_listing_copy(
        owner_id=uuid4(),
        idempotency_key="listing-copy-replay",
        request_hash="b" * 64,
    )

    assert reservation.request_id == request_id
    assert reservation.replay is not None
    assert len(reservation.replay) == 3
    assert not any("FROM memberships" in query for query in connection.statements)


@pytest.mark.asyncio
async def test_member_listing_copy_is_unmetered_and_concurrency_still_applies(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    member_connection = _LedgerConnection(member=True, used=200)
    monkeypatch.setattr(listing_service, "get_engine", lambda: _Engine(member_connection))
    monkeypatch.setattr(listing_service, "get_settings", _settings)

    reservation = await listing_service.reserve_listing_copy(
        owner_id=uuid4(),
        idempotency_key="member-listing-copy",
        request_hash="c" * 64,
    )

    assert reservation.replay is None
    insert_index = next(
        index
        for index, query in enumerate(member_connection.statements)
        if "INSERT INTO ai_requests" in query
    )
    assert member_connection.parameters[insert_index]["counts_toward_trial"] is False

    busy_connection = _LedgerConnection(member=True, active=2)
    monkeypatch.setattr(listing_service, "get_engine", lambda: _Engine(busy_connection))
    with pytest.raises(HTTPException) as raised:
        await listing_service.reserve_listing_copy(
            owner_id=uuid4(),
            idempotency_key="member-busy",
            request_hash="d" * 64,
        )
    assert raised.value.status_code == 429


class _SourceConnection:
    def __init__(self, listing_row, source_row=None) -> None:
        self.listing_row = listing_row
        self.source_row = source_row
        self.calls = 0
        self.statements: list[tuple[str, dict[str, object]]] = []

    async def execute(self, statement, parameters):
        query = " ".join(str(statement).split())
        self.statements.append((query, parameters))
        self.calls += 1
        return _Result(row=self.listing_row if self.calls == 1 else self.source_row)


@pytest.mark.asyncio
async def test_listing_source_owner_predicate_and_revision_owner_scope_fail_closed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owner_id, listing_id, source_id = uuid4(), uuid4(), uuid4()
    row = {
        "id": listing_id,
        "asset_id": uuid4(),
        "status": "draft",
        "title": "Draft",
        "description": "",
        "asset_status": "ready",
        "canonical_spec": {"version": 1},
        "output_spec_json": {"width": 1024, "height": 1024},
        "tags": [],
        "preview_object_key": "private/thumbnail.png",
    }
    connection = _SourceConnection(row, source_row=None)
    monkeypatch.setattr(listing_service, "get_engine", lambda: _Engine(connection))

    with pytest.raises(HTTPException) as raised:
        await listing_service.load_listing_source(
            owner_id=owner_id,
            listing_id=listing_id,
            source_request_id=source_id,
            locale="zh",
        )

    assert raised.value.status_code == 404
    listing_query, listing_parameters = connection.statements[0]
    assert "l.creator_id=:owner_id" in listing_query
    assert listing_parameters["owner_id"] == owner_id
    source_query, source_parameters = connection.statements[1]
    assert "owner_id=:owner_id" in source_query
    assert "listing_id=:listing_id" in source_query
    assert "expires_at>now()" in source_query
    assert source_parameters == {
        "request_id": source_id,
        "owner_id": owner_id,
        "listing_id": listing_id,
        "locale": "zh",
    }


def test_listing_copy_migration_persists_only_candidates_and_retains_request_ledger(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    migration = importlib.import_module(
        "migrations.versions.20260810_0021_ai_listing_copy"
    )
    statements: list[str] = []
    monkeypatch.setattr(migration.op, "execute", statements.append)

    migration.upgrade()

    ddl = "\n".join(statements)
    assert "ai_listing_copy_results" in ddl
    assert "candidates jsonb" in ddl
    assert "REFERENCES ai_requests(id)" in ddl
    assert "expires_at" in ddl
    assert "image" not in ddl.lower()
    assert "api_key" not in ddl.lower()
