"""SiliconFlow OpenAI-compatible streaming client. Secrets never enter logs or results."""
from __future__ import annotations

import json
import re
from collections.abc import AsyncIterator
from copy import deepcopy
from typing import Literal

import httpx

from app.core.config import get_settings, reveal_secret
from app.studio.compute_request_mapper import PREVIEW_MAX_ITERATIONS


class ProviderUnavailable(Exception):
    def __init__(self, reason: str, *, retryable: bool = False) -> None:
        super().__init__(reason)
        self.retryable = retryable


SYSTEM_PROMPT = f"""你是 Fractal Studio 的探索副驾驶。用用户所用语言简洁回答。
图像是视觉判断的最终证据：只描述实际可见内容；没有求解器证据时，不得断言周期、特殊点、
对称阶数或稀有性。不要仅凭亮暗推断迭代次数或内外部；palette 与均衡会改变亮度。
公共 variant 名可能与数学名称不一致：celtic 实为 Perpendicular Burning Ship，buffalo
实为 Celtic，celtic_ship 实为 Buffalo；不确定时只使用 API 名和视觉描述。
项目合同：`metric` 是独立字段；`orbitProgram` 仅在 `metric="escape"` 时可用，`min_abs`
是另一种 metric，绝不是 escape 的参数。冲突时只能把 metric 改回 escape，或移除
orbitProgram。axis transition 不能配 Julia/orbitProgram；自定义 colorProgram 只能配
colorMode="direct"；eq_full/eq_center 只对 escape 生效。当前预览把 iterations 截至
{PREVIEW_MAX_ITERATIONS}。escape 自定义色带使用 (iter+1)/(iterations+2)，预览映射会自动把
cycles 乘以 (预览 iterations+2)/(母版 iterations+2) 来保持色带；不要再次手工补偿。连续场 metric 中
bailout 同时缩放着色输入；不要把这条套用到 escape。
escape metric 会把 smooth 强制为 false，因此绝不能建议为 escape 开启 smooth。
内置色板锚点：viridis=深紫→蓝→青绿→黄绿；ember_blue=深海军蓝→蓝→青→橙→乳白；
spectral1530=深海军蓝→蓝→青绿→黄→红→洋红；twilight=浅灰紫→紫→深蓝→酒红→浅灰紫。
你不能执行渲染、预览、下载、支付、会员变更或联网操作。需要修改 Studio 时只能调用
propose_studio_patch；patch 只包含确实需要改变的字段并尊重 capabilities。建议等待用户确认，
不要声称已经应用。用户只要求调色时不得改变坐标、尺度、公式或迭代数。"""

TOOL = {
    "type": "function",
    "function": {
        "name": "propose_studio_patch",
        "description": "向用户建议一组 Studio 参数修改，等待用户确认后才应用",
        "parameters": {
            "type": "object",
            "properties": {
                "patch": {
                    "type": "object",
                    "properties": {
                        "centerRe": {"type": "number"}, "centerIm": {"type": "number"},
                        "scale": {"type": "number", "exclusiveMinimum": 0},
                        "iterations": {"type": "integer", "minimum": 1, "maximum": 1000000},
                        "variant": {"type": "string"}, "colorMap": {"type": "string"},
                        "metric": {"type": "string"}, "smooth": {"type": "boolean"},
                        "colorMode": {"type": "string"},
                        "cyclesPerOctave": {"type": "number", "exclusiveMinimum": 0, "maximum": 64},
                        "rotationDeg": {"type": "number", "minimum": -360, "maximum": 360},
                        "pairwiseCap": {"type": "integer", "minimum": 1, "maximum": 1000000},
                        "julia": {"type": "boolean"}, "juliaRe": {"type": "number"},
                        "juliaIm": {"type": "number"},
                        "bailout": {"type": "number", "exclusiveMinimum": 0},
                        "engine": {"type": "string"}, "scalarType": {"type": "string"},
                        "transitionMode": {"type": "string", "enum": ["off", "pair", "multi"]},
                        "transitionThetaMilliDeg": {"type": "integer", "minimum": -180000, "maximum": 180000}
                    },
                    "additionalProperties": False
                },
                "reason": {
                    "type": "string",
                    "description": "简述图片中实际可见的配色依据，以及每个变化字段为何改善目标风格"
                },
            },
            "required": ["patch", "reason"],
        },
    },
}


AssistantMode = Literal["chat", "location", "color", "composition"]


def _candidate_item(properties: dict[str, object], required: list[str]) -> dict[str, object]:
    return {
        "type": "object",
        "properties": {
            "label": {
                "type": "string",
                "minLength": 1,
                "maxLength": 60,
                "pattern": "^[^\\\"\\r\\n]+$",
                "description": "使用用户当前语言的短标签，不含引号或换行",
            },
            "reason": {
                "type": "string",
                "minLength": 1,
                "maxLength": 160,
                "pattern": "^[^\\\"\\r\\n]+$",
                "description": (
                    "用用户当前语言写一句短理由，引用图片中真实可见结构解释差异；"
                    "不得使用引号、换行、Markdown 或 JSON 片段"
                ),
            },
            **properties,
        },
        "required": ["label", "reason", *required],
        "additionalProperties": False,
    }


def _navigation_tool(mode: Literal["location", "composition"]) -> dict[str, object]:
    if mode == "location":
        item = _candidate_item(
            {
                "offsetX": {
                    "type": "number", "minimum": -0.45, "maximum": 0.45,
                    "description": "新视口中心向右为正；不是主体在屏幕上的移动方向",
                },
                "offsetY": {
                    "type": "number", "minimum": -0.45, "maximum": 0.45,
                    "description": "新视口中心向上为正；不是主体在屏幕上的移动方向",
                },
                "scaleFactor": {
                    "type": "number", "const": 1,
                    "description": "位置探索固定为1；缩放由构图助手负责",
                },
            },
            ["offsetX", "offsetY", "scaleFactor"],
        )
        properties: dict[str, object] = {
            "axis": {
                "type": "string",
                "const": "position",
                "description": "位置探索固定使用 position；缩放和旋转由构图助手负责。",
            },
            "candidates": {"type": "array", "minItems": 3, "maxItems": 3, "items": item},
        }
        required = ["axis", "candidates"]
        description = (
            "提出三个相对当前画面的定位候选。offsetX/offsetY 是以视口宽高为单位的归一化偏移，"
            "正 Y 向上；当前基准会单独展示，三个候选都必须有非零且彼此明显不同的变化；"
            "axis 必须为 position，三个 scaleFactor 必须精确为 1；缩放和旋转留给构图助手。"
            "不要计算或返回绝对复坐标。JSON 数字的小数点必须用英文句点，例如 0.25，禁止写 0,25。"
        )
    else:
        item = _candidate_item(
            {
                "offsetX": {
                    "type": "number", "minimum": -0.25, "maximum": 0.25,
                    "description": "新视口中心向右为正；主体在屏幕上会相对向左",
                },
                "offsetY": {
                    "type": "number", "minimum": -0.25, "maximum": 0.25,
                    "description": "新视口中心向上为正；主体在屏幕上会相对向下",
                },
                "scaleFactor": {
                    "type": "number", "minimum": 0.75, "maximum": 1.33,
                    "description": "小于1是放大收紧，大于1是缩小视图以显示更多周边",
                },
                "rotationDelta": {"type": "number", "minimum": -30, "maximum": 30},
            },
            ["offsetX", "offsetY", "scaleFactor", "rotationDelta"],
        )
        properties = {
            "candidates": {"type": "array", "minItems": 3, "maxItems": 3, "items": item},
        }
        required = ["candidates"]
        description = (
            "提出三个构图候选，只用相对平移、缩放与旋转；offsetX/offsetY 以视口宽高为单位，"
            "正 Y 向上。当前基准会单独展示，所以三个候选都必须有非零变化且彼此明显不同。"
            "不要计算或返回绝对复坐标，也不要改变颜色、公式或计算参数。"
            "JSON 数字的小数点必须用英文句点，例如 0.25，禁止写 0,25。"
        )
    return {
        "type": "function",
        "function": {
            "name": "propose_studio_patch",
            "description": description,
            "parameters": {
                "type": "object",
                "properties": properties,
                "required": required,
                "additionalProperties": False,
            },
        },
    }


def _color_tool(context: dict) -> dict[str, object]:
    capabilities = context.get("capabilities") if isinstance(context.get("capabilities"), dict) else {}
    patch_properties: dict[str, object] = {
        "metric": {"type": "string"},
        "smooth": {"type": "boolean"},
        "colorMode": {"type": "string"},
        "colorMap": {"type": "string"},
        "cyclesPerOctave": {"type": "number", "exclusiveMinimum": 0, "maximum": 64},
        "bailout": {"type": "number", "exclusiveMinimum": 0, "maximum": 1_000_000_000},
    }
    for field, capability in {
        "metric": "metrics",
        "colorMap": "colorMaps",
        "colorMode": "colorModes",
    }.items():
        allowed = capabilities.get(capability) if isinstance(capabilities, dict) else None
        if isinstance(allowed, list) and allowed:
            patch_properties[field]["enum"] = list(allowed)
    spec = context.get("spec") if isinstance(context.get("spec"), dict) else {}
    if spec.get("metric", "escape") == "escape":
        patch_properties["smooth"] = {"type": "boolean", "const": False}
    gradient = capabilities.get("customGradient") if isinstance(capabilities, dict) else None
    if isinstance(gradient, dict) and gradient.get("enabled") is True:
        maximum_stops = min(6, int(gradient.get("maxStops") or 0))
        if maximum_stops >= 2:
            patch_properties["colorProgram"] = {
                "type": "object",
                "properties": {
                    "schemaVersion": {"type": "integer", "const": 1},
                    "type": {"type": "string", "const": "gradient"},
                    "interpolation": {"type": "string", "const": "rgb"},
                    "wrap": {"type": "string", "enum": ["clamp", "repeat", "mirror"]},
                    "cycles": {"type": "number", "exclusiveMinimum": 0, "maximum": 256},
                    "phase": {"type": "number"},
                    "interiorColor": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}$"},
                    "invalidColor": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}$"},
                    "stops": {
                        "type": "array",
                        "minItems": 2,
                        "maxItems": maximum_stops,
                        "items": {
                            "type": "object",
                            "properties": {
                                "at": {"type": "number", "minimum": 0, "maximum": 1},
                                "color": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}$"},
                            },
                            "required": ["at", "color"],
                            "additionalProperties": False,
                        },
                    },
                },
                "required": ["wrap", "cycles", "phase", "interiorColor", "invalidColor", "stops"],
                "additionalProperties": False,
            }
    item = _candidate_item(
        {
            "patch": {
                "type": "object",
                "properties": patch_properties,
                "minProperties": 1,
                "additionalProperties": False,
            },
        },
        ["patch"],
    )
    return {
        "type": "function",
        "function": {
            "name": "propose_studio_patch",
            "description": (
                "根据当前图片提出四个视觉上明显不同的调色候选。只改变调色字段，"
                "不得改变位置、尺度、旋转、公式或迭代数。自定义 colorProgram 与 colorMap 互斥。"
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "candidates": {"type": "array", "minItems": 4, "maxItems": 4, "items": item},
                },
                "required": ["candidates"],
                "additionalProperties": False,
            },
        },
    }


def _tool_for_context(context: dict, assistant_mode: AssistantMode = "chat") -> dict:
    if assistant_mode in {"location", "composition"}:
        return _navigation_tool(assistant_mode)
    if assistant_mode == "color":
        return _color_tool(context)
    tool = deepcopy(TOOL)
    properties = tool["function"]["parameters"]["properties"]["patch"]["properties"]
    capabilities = context.get("capabilities") if isinstance(context.get("capabilities"), dict) else {}
    for field, capability in {
        "variant": "variants", "colorMap": "colorMaps", "metric": "metrics",
        "colorMode": "colorModes", "engine": "engines", "scalarType": "scalars",
    }.items():
        allowed = capabilities.get(capability) if isinstance(capabilities, dict) else None
        if isinstance(allowed, list) and allowed:
            properties[field]["enum"] = allowed
    spec = context.get("spec") if isinstance(context.get("spec"), dict) else {}
    if spec.get("metric", "escape") == "escape":
        properties["smooth"] = {"type": "boolean", "const": False}
    if not context.get("member"):
        properties["transitionMode"]["enum"] = ["off", "pair"]
    return tool


def _stream_error_is_retryable(chunk: dict[str, object]) -> bool:
    """Recognize only sanitized HTTP-equivalent retry signals from an SSE error frame."""

    error = chunk.get("error")
    candidates: list[object] = [chunk.get("status"), chunk.get("status_code")]
    if isinstance(error, dict):
        candidates.extend((error.get("status"), error.get("status_code"), error.get("code")))
    for candidate in candidates:
        if isinstance(candidate, bool):
            continue
        if isinstance(candidate, int) and candidate in {429, 503}:
            return True
        if isinstance(candidate, str) and candidate.strip() in {"429", "503"}:
            return True
    return False


def _decode_tool_arguments(arguments: str) -> object:
    """Decode provider tool JSON, tolerating only literal control characters.

    Some OpenAI-compatible model streams leave a literal newline inside a
    quoted reason, a trailing comma, or use a locale decimal comma for a
    numeric property. The narrow fallbacks below handle only those syntax
    defects; every resulting object still goes through the full Platform
    validator. We deliberately do not guess at missing braces, quotes,
    property names or fields.
    """

    try:
        return json.loads(arguments)
    except json.JSONDecodeError as strict_error:
        try:
            return json.loads(arguments, strict=False)
        except json.JSONDecodeError:
            normalized_numbers = re.sub(
                r"(:\s*-?\d+),(\d+)(?=\s*[,}])", r"\1.\2", arguments
            )
            normalized = re.sub(r",(\s*[}\]])", r"\1", normalized_numbers)
            if normalized != arguments:
                try:
                    return json.loads(normalized, strict=False)
                except json.JSONDecodeError:
                    pass
        raise ProviderUnavailable(
            "invalid provider tool arguments "
            f"kind={strict_error.msg} length={len(arguments)} pos={strict_error.pos}"
        ) from None


async def stream_completion(*, text: str, history: list[dict], context: dict, image: bytes | None,
                            image_type: str | None, force_patch: bool = False,
                            disable_tools: bool = False,
                            assistant_mode: AssistantMode = "chat") -> AsyncIterator[tuple[str, object]]:
    settings = get_settings()
    content: object = text
    if image:
        import base64
        content = [
            {"type": "text", "text": text},
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:{image_type};base64,{base64.b64encode(image).decode()}",
                    "detail": "high",
                },
            },
        ]
    messages = [{"role": "system", "content": SYSTEM_PROMPT + "\n当前上下文：" + json.dumps(context, ensure_ascii=False)}]
    messages.extend(history[-20:])
    messages.append({"role": "user", "content": content})
    tool_choice: object = (
        {"type": "function", "function": {"name": "propose_studio_patch"}}
        if force_patch and "-VL-" not in settings.siliconflow_model else "auto"
    )
    payload = {"model": settings.siliconflow_model, "messages": messages, "stream": True,
               "max_tokens": settings.ai_max_output_tokens,
               "stream_options": {"include_usage": True},
               # Structured exploration must be reproducible: even a small
               # sampling temperature occasionally makes Qwen emit malformed
               # tool arguments that the Platform correctly rejects.
               "temperature": 0 if force_patch else 0.2}
    # SiliconFlow's Qwen3-VL Instruct endpoints reject the text-model-only
    # `enable_thinking` extension. The Instruct variants are already the
    # non-thinking models, so omit the unsupported field for those endpoints.
    if "-VL-" not in settings.siliconflow_model:
        payload["enable_thinking"] = False
    if not disable_tools:
        payload.update({"tools": [_tool_for_context(context, assistant_mode)], "tool_choice": tool_choice})
    tool_calls: dict[int, dict[str, str]] = {}
    timeout = httpx.Timeout(connect=10, read=90, write=20, pool=10)
    api_key = reveal_secret(settings.siliconflow_api_key)
    try:
        async with httpx.AsyncClient(base_url=settings.siliconflow_base_url, trust_env=False, timeout=timeout) as client:
            async with client.stream("POST", "/chat/completions", json=payload,
                                     headers={"Authorization": f"Bearer {api_key}"}) as response:
                if response.status_code >= 400:
                    raise ProviderUnavailable(
                        f"provider status {response.status_code}",
                        retryable=response.status_code in {429, 503},
                    )
                async for line in response.aiter_lines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].lstrip()
                    if not data or data == "[DONE]":
                        continue
                    chunk = json.loads(data)
                    if not isinstance(chunk, dict):
                        raise ProviderUnavailable("invalid provider stream chunk")
                    if chunk.get("error") is not None:
                        # Never interpolate the provider's error body. It may
                        # contain credentials, request content or private image
                        # diagnostics. The service owns retry/partial semantics.
                        raise ProviderUnavailable(
                            "provider stream error",
                            retryable=_stream_error_is_retryable(chunk),
                        )
                    if chunk.get("usage"):
                        if not isinstance(chunk["usage"], dict):
                            raise ProviderUnavailable("invalid provider usage")
                        yield "usage", chunk["usage"]
                    choices = chunk.get("choices") or []
                    if not isinstance(choices, list) or not choices:
                        continue
                    choice = choices[0]
                    if not isinstance(choice, dict):
                        raise ProviderUnavailable("invalid provider stream choice")
                    delta = choice.get("delta") or {}
                    if not isinstance(delta, dict):
                        raise ProviderUnavailable("invalid provider stream delta")
                    if delta.get("content"):
                        if not isinstance(delta["content"], str):
                            raise ProviderUnavailable("invalid provider content")
                        yield "delta", delta["content"]
                    calls = delta.get("tool_calls") or []
                    if not isinstance(calls, list):
                        raise ProviderUnavailable("invalid provider tool calls")
                    for call in calls:
                        if not isinstance(call, dict):
                            raise ProviderUnavailable("invalid provider tool call")
                        try:
                            index = int(call.get("index", 0))
                        except (TypeError, ValueError, OverflowError):
                            raise ProviderUnavailable("invalid provider tool call index") from None
                        function = call.get("function") or {}
                        if not isinstance(function, dict):
                            raise ProviderUnavailable("invalid provider tool function")
                        aggregate = tool_calls.setdefault(index, {"name": "", "arguments": ""})
                        # SiliconFlow sends the name once, then name="" on argument chunks.
                        if function.get("name"):
                            if not isinstance(function["name"], str):
                                raise ProviderUnavailable("invalid provider tool name")
                            aggregate["name"] = function["name"]
                        arguments = function.get("arguments") or ""
                        if not isinstance(arguments, str):
                            raise ProviderUnavailable("invalid provider tool arguments")
                        aggregate["arguments"] += arguments
    except ProviderUnavailable:
        raise
    except httpx.HTTPError as error:
        raise ProviderUnavailable(type(error).__name__, retryable=True) from None
    except json.JSONDecodeError as error:
        raise ProviderUnavailable(type(error).__name__) from None
    for call in tool_calls.values():
        if call["name"] != "propose_studio_patch" or not call["arguments"]:
            continue
        yield "suggestion", _decode_tool_arguments(call["arguments"])
