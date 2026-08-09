"""Security contracts for the Studio AI message HTTP boundary."""

from __future__ import annotations

import json
from types import SimpleNamespace
from uuid import uuid4

import pytest
from fastapi import HTTPException
from starlette.requests import Request

import app.ai.router as ai_router
from app.ai.models import validate_studio_suggestion
from app.auth.models import AccessPrincipal
from app.infrastructure.compute.compute_client import ComputeClientError


class _MembershipConnection:
    def __init__(self, *, active: bool, owner_id) -> None:
        self.active = active
        self.owner_id = owner_id

    async def scalar(self, statement, parameters):
        assert "FROM memberships" in str(statement)
        assert "status='active'" in str(statement)
        assert parameters == {"owner": self.owner_id}
        return 1 if self.active else None


class _ConnectionContext:
    def __init__(self, connection: _MembershipConnection) -> None:
        self.connection = connection

    async def __aenter__(self) -> _MembershipConnection:
        return self.connection

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        return None


class _MembershipEngine:
    def __init__(self, *, active: bool, owner_id) -> None:
        self.connection = _MembershipConnection(active=active, owner_id=owner_id)

    def connect(self) -> _ConnectionContext:
        return _ConnectionContext(self.connection)


def _request() -> Request:
    return Request(
        {
            "type": "http",
            "http_version": "1.1",
            "method": "POST",
            "scheme": "https",
            "path": "/v1/ai/conversations/test/messages",
            "raw_path": b"/v1/ai/conversations/test/messages",
            "query_string": b"",
            "headers": [],
            "client": ("127.0.0.1", 12345),
            "server": ("testserver", 443),
        }
    )


def _principal() -> AccessPrincipal:
    return AccessPrincipal(
        user_id=uuid4(),
        session_id=uuid4(),
        roles=frozenset(),
        session_token="test-session",
    )


def _install_common(monkeypatch: pytest.MonkeyPatch, principal: AccessPrincipal, *, member: bool):
    monkeypatch.setattr(
        ai_router,
        "get_settings",
        lambda: SimpleNamespace(ai_enabled=True, ai_max_user_message_chars=4000),
    )
    monkeypatch.setattr(ai_router, "enforce_origin_and_csrf", lambda request, actor: None)
    monkeypatch.setattr(
        ai_router,
        "get_engine",
        lambda: _MembershipEngine(active=member, owner_id=principal.user_id),
    )


@pytest.mark.parametrize("constant", ["NaN", "Infinity", "-Infinity"])
def test_context_rejects_non_finite_json_numbers(constant: str) -> None:
    with pytest.raises(HTTPException) as raised:
        ai_router._parse_context(f'{{"scale":{constant}}}')

    assert raised.value.status_code == 422
    assert raised.value.detail == "validation_error"


@pytest.mark.asyncio
async def test_message_overwrites_forged_member_and_capabilities(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    principal = _principal()
    _install_common(monkeypatch, principal, member=False)
    trusted_capabilities = {
        "variants": ["mandelbrot"],
        "colorMaps": ["inferno"],
        "metrics": ["escape"],
    }
    captured: dict[str, object] = {}

    async def capabilities() -> dict[str, object]:
        return trusted_capabilities

    async def stream_message(**kwargs):
        captured.update(kwargs)
        yield b"event: message\ndata: {}\n\n"

    monkeypatch.setattr(ai_router, "studio_capabilities", capabilities)
    monkeypatch.setattr(ai_router, "stream_message", stream_message)

    response = await ai_router.post_message(
        conversation_id=uuid4(),
        request=_request(),
        text="解释当前参数",
        context=json.dumps(
            {
                "member": True,
                "capabilities": {
                    "variants": ["forged-variant"],
                    "colorMaps": ["forged-palette"],
                },
                "spec": {"version": 1, "variant": "mandelbrot"},
            }
        ),
        request_patch=False,
        image=None,
        idempotency_key="message-1",
        principal=principal,
    )

    assert response.media_type == "text/event-stream"
    assert captured["owner_id"] == principal.user_id
    assert captured["force_patch"] is False
    assert captured["context"] == {
        "member": False,
        "capabilities": trusted_capabilities,
        "spec": {"version": 1, "variant": "mandelbrot"},
    }
    assert validate_studio_suggestion(
        {"patch": {"transitionMode": "multi"}, "reason": "member only"},
        captured["context"],
    ) is None


@pytest.mark.asyncio
async def test_plain_question_continues_with_empty_capabilities_when_compute_is_unavailable(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    principal = _principal()
    _install_common(monkeypatch, principal, member=True)
    captured: dict[str, object] = {}

    async def unavailable() -> dict[str, object]:
        raise ComputeClientError("compute_unavailable")

    async def stream_message(**kwargs):
        captured.update(kwargs)
        yield b"event: message\ndata: {}\n\n"

    monkeypatch.setattr(ai_router, "studio_capabilities", unavailable)
    monkeypatch.setattr(ai_router, "stream_message", stream_message)

    response = await ai_router.post_message(
        conversation_id=uuid4(),
        request=_request(),
        text="什么是逃逸时间算法？",
        context=json.dumps(
            {
                "member": False,
                "capabilities": {"variants": ["forged-variant"]},
                "spec": {"version": 1},
            }
        ),
        request_patch=False,
        image=None,
        idempotency_key="message-2",
        principal=principal,
    )

    assert response.media_type == "text/event-stream"
    assert captured["context"] == {
        "member": True,
        "capabilities": {},
        "spec": {"version": 1},
    }
    assert validate_studio_suggestion(
        {"patch": {"variant": "forged-variant"}, "reason": "untrusted"},
        captured["context"],
    ) is None


@pytest.mark.asyncio
async def test_forced_patch_fails_before_provider_when_compute_capabilities_are_unavailable(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    principal = _principal()
    _install_common(monkeypatch, principal, member=False)
    provider_called = False

    async def unavailable() -> dict[str, object]:
        raise ComputeClientError("compute_capacity_exhausted")

    async def stream_message(**kwargs):
        nonlocal provider_called
        provider_called = True
        yield b"event: message\ndata: {}\n\n"

    monkeypatch.setattr(ai_router, "studio_capabilities", unavailable)
    monkeypatch.setattr(ai_router, "stream_message", stream_message)

    with pytest.raises(HTTPException) as raised:
        await ai_router.post_message(
            conversation_id=uuid4(),
            request=_request(),
            text="给我一个配色建议",
            context=json.dumps(
                {
                    "member": True,
                    "capabilities": {
                        "variants": ["forged-variant"],
                        "colorMaps": ["forged-palette"],
                    },
                }
            ),
            request_patch=True,
            image=None,
            idempotency_key="message-3",
            principal=principal,
        )

    assert raised.value.status_code == 503
    assert raised.value.detail == "COMPUTE_CAPACITY_EXHAUSTED"
    assert provider_called is False


@pytest.mark.asyncio
async def test_streaming_response_closes_service_iterator_when_client_stops_after_first_frame(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    principal = _principal()
    _install_common(monkeypatch, principal, member=False)
    upstream_closed = False

    async def capabilities() -> dict[str, object]:
        return {"variants": ["mandelbrot"]}

    async def stream_message(**_kwargs):
        nonlocal upstream_closed
        try:
            yield b"event: message\ndata: {}\n\n"
            yield b"event: delta\ndata: {\"content\":\"late\"}\n\n"
        finally:
            upstream_closed = True

    monkeypatch.setattr(ai_router, "studio_capabilities", capabilities)
    monkeypatch.setattr(ai_router, "stream_message", stream_message)
    response = await ai_router.post_message(
        conversation_id=uuid4(),
        request=_request(),
        text="stop immediately",
        context="{}",
        request_patch=False,
        image=None,
        idempotency_key="stop-after-first-frame",
        principal=principal,
    )

    assert await anext(response.body_iterator) == b"event: message\ndata: {}\n\n"
    await response.body_iterator.aclose()

    assert upstream_closed is True
