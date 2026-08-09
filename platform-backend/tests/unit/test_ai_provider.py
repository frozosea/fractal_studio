"""Deterministic protocol and secret-handling tests for the AI provider client."""

from __future__ import annotations

import base64
import json
from types import SimpleNamespace
from typing import Any

import httpx
import pytest

from app.ai import provider


def _settings(
    *,
    api_key: str = "sf-test-secret-never-log",
    model: str = "test/model",
) -> SimpleNamespace:
    return SimpleNamespace(
        siliconflow_api_key=api_key,
        siliconflow_base_url="https://provider.invalid/v1",
        siliconflow_model=model,
        ai_max_output_tokens=321,
    )


def _sse(*chunks: dict[str, Any]) -> bytes:
    events = [f"data: {json.dumps(chunk, ensure_ascii=False)}\n\n" for chunk in chunks]
    events.append("data: [DONE]\n\n")
    return "".join(events).encode()


def _install_transport(
    monkeypatch: pytest.MonkeyPatch,
    handler: httpx.AsyncBaseTransport,
) -> dict[str, Any]:
    """Route the hard-coded client through an in-process transport and record its options."""

    real_async_client = httpx.AsyncClient
    recorded: dict[str, Any] = {}

    def client_factory(**kwargs: Any) -> httpx.AsyncClient:
        recorded["client_kwargs"] = kwargs.copy()
        return real_async_client(transport=handler, **kwargs)

    monkeypatch.setattr(provider.httpx, "AsyncClient", client_factory)
    return recorded


@pytest.mark.asyncio
async def test_forced_patch_sends_bounded_history_image_and_assembles_siliconflow_tool_chunks(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    settings = _settings()
    captured: dict[str, Any] = {}
    first_tool_fragment = '{"patch":{"palette":"sun'
    second_tool_fragment = 'set"},"reason":"暖色更突出层次"}'

    async def handle(request: httpx.Request) -> httpx.Response:
        captured["request"] = request
        captured["payload"] = json.loads(request.content)
        return httpx.Response(
            200,
            headers={"content-type": "text/event-stream"},
            content=_sse(
                {"choices": [{"delta": {"content": "先看这张图。"}}]},
                {
                    "choices": [
                        {
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "function": {
                                            "name": "propose_studio_patch",
                                            "arguments": first_tool_fragment,
                                        },
                                    }
                                ]
                            }
                        }
                    ]
                },
                {
                    "choices": [
                        {
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "function": {
                                            "name": "",
                                            "arguments": second_tool_fragment,
                                        },
                                    },
                                    {
                                        "index": 1,
                                        "function": {
                                            "name": "trigger_render",
                                            "arguments": (
                                                '{"patch":{"iterations":999999},'
                                                '"reason":"must be ignored"}'
                                            ),
                                        },
                                    },
                                ]
                            }
                        }
                    ]
                },
                {
                    "choices": [],
                    "usage": {"prompt_tokens": 20, "completion_tokens": 8, "total_tokens": 28},
                },
            ),
            request=request,
        )

    recorded = _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)
    history = [{"role": "user", "content": f"history-{index}"} for index in range(22)]
    image = b"small-preview"

    events = [
        event
        async for event in provider.stream_completion(
            text="分析当前作品",
            history=history,
            context={"mode": "explore", "capabilities": {"cuda": True}},
            image=image,
            image_type="image/png",
            force_patch=True,
        )
    ]

    assert recorded["client_kwargs"]["trust_env"] is False
    assert recorded["client_kwargs"]["base_url"] == settings.siliconflow_base_url
    request = captured["request"]
    assert request.headers["authorization"] == f"Bearer {settings.siliconflow_api_key}"
    payload = captured["payload"]
    assert payload["model"] == settings.siliconflow_model
    assert payload["max_tokens"] == 321
    assert payload["stream"] is True
    assert payload["stream_options"] == {"include_usage": True}
    assert payload["tool_choice"] == {
        "type": "function",
        "function": {"name": "propose_studio_patch"},
    }
    assert payload["enable_thinking"] is False
    assert payload["temperature"] == 0.2
    assert payload["tools"][0]["function"]["name"] == "propose_studio_patch"
    assert len(payload["messages"]) == 22
    assert payload["messages"][1]["content"] == "history-2"
    assert payload["messages"][-2]["content"] == "history-21"
    assert '"mode": "explore"' in payload["messages"][0]["content"]
    assert payload["messages"][-1] == {
        "role": "user",
        "content": [
            {"type": "text", "text": "分析当前作品"},
            {
                "type": "image_url",
                "image_url": {
                    "url": "data:image/png;base64," + base64.b64encode(image).decode(),
                    "detail": "high",
                },
            },
        ],
    }
    assert settings.siliconflow_api_key.encode() not in request.content
    assert events == [
        ("delta", "先看这张图。"),
        ("usage", {"prompt_tokens": 20, "completion_tokens": 8, "total_tokens": 28}),
        (
            "suggestion",
            {"patch": {"palette": "sunset"}, "reason": "暖色更突出层次"},
        ),
    ]


@pytest.mark.asyncio
async def test_normal_text_request_streams_visible_content_with_auto_tool_choice(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    settings = _settings()
    captured: dict[str, Any] = {}

    async def handle(request: httpx.Request) -> httpx.Response:
        captured["payload"] = json.loads(request.content)
        return httpx.Response(
            200,
            headers={"content-type": "text/event-stream"},
            content=_sse(
                {"choices": [{"delta": {"content": "边界附近的轨道"}}]},
                {"choices": [{"delta": {"content": "对初值极其敏感。"}}]},
                {
                    "choices": [],
                    "usage": {"prompt_tokens": 12, "completion_tokens": 9, "total_tokens": 21},
                },
            ),
            request=request,
        )

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)

    events = [
        event
        async for event in provider.stream_completion(
            text="为什么边界细节丰富？",
            history=[],
            context={"mode": "map"},
            image=None,
            image_type=None,
            force_patch=False,
        )
    ]

    payload = captured["payload"]
    assert payload["tool_choice"] == "auto"
    assert payload["enable_thinking"] is False
    assert payload["messages"][-1] == {
        "role": "user",
        "content": "为什么边界细节丰富？",
    }
    assert events == [
        ("delta", "边界附近的轨道"),
        ("delta", "对初值极其敏感。"),
        ("usage", {"prompt_tokens": 12, "completion_tokens": 9, "total_tokens": 21}),
    ]


@pytest.mark.asyncio
async def test_analysis_phase_omits_tools_entirely(monkeypatch: pytest.MonkeyPatch) -> None:
    settings = _settings(model="Qwen/Qwen3-VL-32B-Instruct")
    captured: dict[str, Any] = {}

    async def handle(request: httpx.Request) -> httpx.Response:
        captured["payload"] = json.loads(request.content)
        return httpx.Response(
            200,
            content=_sse(
                {"choices": [{"delta": {"content": "只描述可见结构"}}]},
                {"choices": [], "usage": {"total_tokens": 12}},
            ),
            request=request,
        )

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)
    events = [
        event
        async for event in provider.stream_completion(
            text="分析图片", history=[], context={}, image=b"png", image_type="image/png",
            disable_tools=True,
        )
    ]
    assert "tools" not in captured["payload"]
    assert "tool_choice" not in captured["payload"]
    assert "enable_thinking" not in captured["payload"]
    assert events[0] == ("delta", "只描述可见结构")


@pytest.mark.asyncio
async def test_vl_forced_patch_uses_supported_auto_tool_choice(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    settings = _settings(model="Qwen/Qwen3-VL-30B-A3B-Instruct")
    captured: dict[str, Any] = {}

    async def handle(request: httpx.Request) -> httpx.Response:
        captured["payload"] = json.loads(request.content)
        return httpx.Response(200, content=_sse({"choices": []}), request=request)

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)
    events = [
        event
        async for event in provider.stream_completion(
            text="提出候选",
            history=[],
            context={},
            image=None,
            image_type=None,
            force_patch=True,
            assistant_mode="composition",
        )
    ]

    assert captured["payload"]["tool_choice"] == "auto"
    assert "enable_thinking" not in captured["payload"]
    assert events == []


def test_contextual_tool_schema_uses_capabilities_and_disables_ineffective_smoothing() -> None:
    tool = provider._tool_for_context(
        {
            "member": False,
            "spec": {"metric": "escape"},
            "capabilities": {
                "colorMaps": ["inferno", "ember_blue"],
                "engines": ["auto", "cuda"],
            },
        }
    )
    properties = tool["function"]["parameters"]["properties"]["patch"]["properties"]
    assert properties["colorMap"]["enum"] == ["inferno", "ember_blue"]
    assert properties["engine"]["enum"] == ["auto", "cuda"]
    assert properties["smooth"] == {"type": "boolean", "const": False}
    assert properties["transitionMode"]["enum"] == ["off", "pair"]


def test_exploration_tool_schemas_are_mode_specific_and_capability_bounded() -> None:
    context = {
        "spec": {"metric": "escape"},
        "capabilities": {
            "metrics": ["escape", "min_abs"],
            "colorMaps": ["inferno", "ember_blue"],
            "colorModes": ["direct", "eq_full"],
            "customGradient": {
                "enabled": True,
                "maxStops": 16,
                "kinds": ["map_image"],
            },
        },
    }
    location = provider._tool_for_context(context, "location")["function"]["parameters"]
    assert location["properties"]["axis"]["enum"] == ["position", "scale"]
    assert location["properties"]["candidates"]["minItems"] == 3
    location_item = location["properties"]["candidates"]["items"]
    assert set(location_item["properties"]) == {
        "label", "reason", "offsetX", "offsetY", "scaleFactor",
    }
    assert "centerRe" not in location_item["properties"]

    composition = provider._tool_for_context(context, "composition")["function"]["parameters"]
    assert composition["properties"]["candidates"]["maxItems"] == 3
    assert "rotationDelta" in composition["properties"]["candidates"]["items"]["properties"]

    color = provider._tool_for_context(context, "color")["function"]["parameters"]
    assert color["properties"]["candidates"]["minItems"] == 4
    patch = color["properties"]["candidates"]["items"]["properties"]["patch"]
    assert patch["properties"]["colorMap"]["enum"] == ["inferno", "ember_blue"]
    assert patch["properties"]["smooth"] == {"type": "boolean", "const": False}
    assert patch["properties"]["colorProgram"]["properties"]["stops"]["maxItems"] == 6
    assert "iterations" not in patch["properties"]


@pytest.mark.asyncio
@pytest.mark.parametrize("status", [401, 429, 500, 502, 503, 504])
async def test_provider_http_failures_are_mapped_without_leaking_the_key(
    monkeypatch: pytest.MonkeyPatch,
    status: int,
) -> None:
    settings = _settings()

    async def handle(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            status,
            content=f"upstream accidentally echoed {settings.siliconflow_api_key}".encode(),
            request=request,
        )

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)

    with pytest.raises(provider.ProviderUnavailable) as raised:
        _ = [
            event
            async for event in provider.stream_completion(
                text="hello", history=[], context={}, image=None, image_type=None
            )
        ]

    assert settings.siliconflow_api_key not in str(raised.value)
    assert raised.value.__cause__ is None or settings.siliconflow_api_key not in str(
        raised.value.__cause__
    )


@pytest.mark.asyncio
async def test_connection_failure_is_mapped_without_exposing_exception_details(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    settings = _settings()

    async def handle(request: httpx.Request) -> httpx.Response:
        raise httpx.ConnectError(
            f"diagnostic contained {settings.siliconflow_api_key}", request=request
        )

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)

    with pytest.raises(provider.ProviderUnavailable) as raised:
        _ = [
            event
            async for event in provider.stream_completion(
                text="hello", history=[], context={}, image=None, image_type=None
            )
        ]

    assert str(raised.value) == "ConnectError"
    assert settings.siliconflow_api_key not in str(raised.value)


@pytest.mark.asyncio
async def test_malformed_stream_is_mapped_and_incomplete_tool_call_is_not_emitted(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    settings = _settings()
    responses = iter(
        [
            b'data: {"choices":\n\ndata: [DONE]\n\n',
            _sse(
                {
                    "choices": [
                        {
                            "delta": {
                                "tool_calls": [
                                    {
                                        "function": {
                                            "name": "propose_studio_patch",
                                            "arguments": '{"patch":',
                                        }
                                    }
                                ]
                            }
                        }
                    ]
                }
            ),
        ]
    )

    async def handle(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, content=next(responses), request=request)

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)

    with pytest.raises(provider.ProviderUnavailable, match="JSONDecodeError"):
        _ = [
            event
            async for event in provider.stream_completion(
                text="hello", history=[], context={}, image=None, image_type=None
            )
        ]

    events = [
        event
        async for event in provider.stream_completion(
            text="hello", history=[], context={}, image=None, image_type=None
        )
    ]
    assert events == []


@pytest.mark.asyncio
async def test_invalid_tool_call_index_is_sanitized(monkeypatch: pytest.MonkeyPatch) -> None:
    settings = _settings()

    async def handle(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200,
            content=_sse({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": {"not": "an integer"},
                            "function": {"name": "propose_studio_patch", "arguments": "{}"},
                        }],
                    },
                }],
            }),
            request=request,
        )

    _install_transport(monkeypatch, httpx.MockTransport(handle))
    monkeypatch.setattr(provider, "get_settings", lambda: settings)
    with pytest.raises(provider.ProviderUnavailable, match="invalid provider tool call index"):
        _ = [
            event
            async for event in provider.stream_completion(
                text="hello", history=[], context={}, image=None, image_type=None
            )
        ]
