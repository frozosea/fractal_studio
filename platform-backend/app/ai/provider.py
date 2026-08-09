"""SiliconFlow OpenAI-compatible streaming client. Secrets never enter logs or results."""
from __future__ import annotations

import json
from copy import deepcopy
from collections.abc import AsyncIterator

import httpx

from app.core.config import get_settings


class ProviderUnavailable(Exception):
    def __init__(self, reason: str, *, retryable: bool = False) -> None:
        super().__init__(reason)
        self.retryable = retryable


SYSTEM_PROMPT = """你是 Fractal Studio 的探索副驾驶。用用户所用语言简洁回答。
图像是视觉判断的最终证据：只描述实际可见内容；没有求解器证据时，不得断言周期、特殊点、
对称阶数或稀有性。不要仅凭亮暗推断迭代次数或内外部；palette 与均衡会改变亮度。
公共 variant 名可能与数学名称不一致：celtic 实为 Perpendicular Burning Ship，buffalo
实为 Celtic，celtic_ship 实为 Buffalo；不确定时只使用 API 名和视觉描述。
项目合同：`metric` 是独立字段；`orbitProgram` 仅在 `metric="escape"` 时可用，`min_abs`
是另一种 metric，绝不是 escape 的参数。冲突时只能把 metric 改回 escape，或移除
orbitProgram。axis transition 不能配 Julia/orbitProgram；自定义 colorProgram 只能配
colorMode="direct"；eq_full/eq_center 只对 escape 生效。预览把 iterations 截至 4096 且
不会自动调整 colorProgram.cycles。escape 自定义色带的 band=iterations/cycles；为了保持
band，预览 cycles = 母版 cycles × 预览 iterations / 母版 iterations。连续场 metric 中
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


def _tool_for_context(context: dict) -> dict:
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


async def stream_completion(*, text: str, history: list[dict], context: dict, image: bytes | None,
                            image_type: str | None, force_patch: bool = False,
                            disable_tools: bool = False) -> AsyncIterator[tuple[str, object]]:
    settings = get_settings()
    content: object = text
    if image:
        import base64
        content = [
            {"type": "text", "text": text},
            {"type": "image_url", "image_url": {"url": f"data:{image_type};base64,{base64.b64encode(image).decode()}"}},
        ]
    messages = [{"role": "system", "content": SYSTEM_PROMPT + "\n当前上下文：" + json.dumps(context, ensure_ascii=False)}]
    messages.extend(history[-20:])
    messages.append({"role": "user", "content": content})
    tool_choice: object = (
        {"type": "function", "function": {"name": "propose_studio_patch"}}
        if force_patch else "auto"
    )
    payload = {"model": settings.siliconflow_model, "messages": messages, "stream": True,
               "max_tokens": settings.ai_max_output_tokens,
               "stream_options": {"include_usage": True}, "enable_thinking": False}
    if not disable_tools:
        payload.update({"tools": [_tool_for_context(context)], "tool_choice": tool_choice})
    tool_calls: dict[int, dict[str, str]] = {}
    timeout = httpx.Timeout(connect=10, read=90, write=20, pool=10)
    try:
        async with httpx.AsyncClient(base_url=settings.siliconflow_base_url, trust_env=False, timeout=timeout) as client:
            async with client.stream("POST", "/chat/completions", json=payload,
                                     headers={"Authorization": f"Bearer {settings.siliconflow_api_key}"}) as response:
                if response.status_code >= 400:
                    raise ProviderUnavailable(
                        f"provider status {response.status_code}",
                        retryable=response.status_code in {429, 503},
                    )
                async for line in response.aiter_lines():
                    if not line.startswith("data: ") or line == "data: [DONE]":
                        continue
                    chunk = json.loads(line[6:])
                    if chunk.get("usage"):
                        yield "usage", chunk["usage"]
                    choices = chunk.get("choices") or []
                    if not choices:
                        continue
                    delta = choices[0].get("delta") or {}
                    if delta.get("content"):
                        yield "delta", delta["content"]
                    for call in delta.get("tool_calls") or []:
                        index = int(call.get("index", 0))
                        function = call.get("function") or {}
                        aggregate = tool_calls.setdefault(index, {"name": "", "arguments": ""})
                        # SiliconFlow sends the name once, then name="" on argument chunks.
                        if function.get("name"):
                            aggregate["name"] = str(function["name"])
                        aggregate["arguments"] += str(function.get("arguments") or "")
    except ProviderUnavailable:
        raise
    except httpx.HTTPError as error:
        raise ProviderUnavailable(type(error).__name__, retryable=True) from None
    except json.JSONDecodeError as error:
        raise ProviderUnavailable(type(error).__name__) from None
    for call in tool_calls.values():
        if call["name"] != "propose_studio_patch" or not call["arguments"]:
            continue
        try:
            yield "suggestion", json.loads(call["arguments"])
        except json.JSONDecodeError:
            continue
