#!/usr/bin/env python3
"""Explicit, low-cost SiliconFlow streaming contract check.

The script only reads credentials from the process environment and never prints
request headers, response bodies, prompts, or the API key. By default it makes
two short requests against SILICONFLOW_MODEL. Use ``--include-candidates`` only
when intentionally comparing the planned model shortlist.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import io
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Any

import httpx
from PIL import Image, ImageDraw


DEFAULT_MODEL = "Qwen/Qwen3.6-35B-A3B"
CANDIDATE_MODELS = (
    DEFAULT_MODEL,
    "Qwen/Qwen3-VL-32B-Instruct",
    "Qwen/Qwen3-VL-30B-A3B-Instruct",
)
TOOL = {
    "type": "function",
    "function": {
        "name": "propose_studio_patch",
        "description": "Suggest a Studio parameter patch that the user may choose to apply.",
        "parameters": {
            "type": "object",
            "properties": {
                "patch": {"type": "object", "additionalProperties": True},
                "reason": {"type": "string"},
            },
            "required": ["patch", "reason"],
        },
    },
}


class ContractFailure(RuntimeError):
    """A sanitized contract failure safe to show in CI output."""


@dataclass
class StreamResult:
    content_parts: list[str] = field(default_factory=list)
    tool_names: dict[int, str] = field(default_factory=dict)
    tool_arguments: dict[int, list[str]] = field(default_factory=dict)
    usage: dict[str, Any] | None = None
    chunks: int = 0
    reasoning_characters: int = 0
    finish_reasons: list[str] = field(default_factory=list)

    @property
    def content(self) -> str:
        return "".join(self.content_parts)


def _bounded_int_env(name: str, default: int, *, minimum: int, maximum: int) -> int:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        value = int(raw)
    except ValueError as error:
        raise ContractFailure(f"{name} must be an integer") from error
    if not minimum <= value <= maximum:
        raise ContractFailure(f"{name} must be between {minimum} and {maximum}")
    return value


def _models(args: argparse.Namespace) -> list[str]:
    requested = list(args.model or [])
    requested.extend(
        model.strip() for model in os.getenv("AI_CONTRACT_MODELS", "").split(",") if model.strip()
    )
    if not requested:
        requested.append(os.getenv("SILICONFLOW_MODEL", DEFAULT_MODEL).strip() or DEFAULT_MODEL)
    if args.include_candidates:
        requested.extend(CANDIDATE_MODELS)
    return list(dict.fromkeys(requested))


def _preview_data_url() -> str:
    """Create a small in-memory preview; no image is written to disk."""

    image = Image.new("RGB", (64, 64), "#111827")
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 31, 31), fill="#1d4ed8")
    draw.rectangle((32, 0, 63, 31), fill="#f59e0b")
    draw.rectangle((0, 32, 31, 63), fill="#7c3aed")
    draw.rectangle((32, 32, 63, 63), fill="#f8fafc")
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    encoded = base64.b64encode(buffer.getvalue()).decode("ascii")
    return f"data:image/png;base64,{encoded}"


def _usage_total(usage: dict[str, Any] | None) -> int:
    if not isinstance(usage, dict):
        raise ContractFailure("stream did not include token usage")
    total = usage.get("total_tokens")
    if not isinstance(total, int) or total <= 0:
        raise ContractFailure("stream returned invalid token usage")
    return total


async def _stream_request(
    client: httpx.AsyncClient,
    *,
    payload: dict[str, Any],
) -> StreamResult:
    result = StreamResult()
    try:
        async with client.stream("POST", "/chat/completions", json=payload) as response:
            if response.status_code != 200:
                await response.aread()
                error_code: object = "unknown"
                try:
                    error_payload = response.json()
                    if isinstance(error_payload, dict) and isinstance(
                        error_payload.get("error"), dict
                    ):
                        error_code = error_payload["error"].get("code", "unknown")
                    elif isinstance(error_payload, dict):
                        error_code = error_payload.get("code", "unknown")
                except (json.JSONDecodeError, UnicodeDecodeError):
                    pass
                raise ContractFailure(
                    f"provider rejected request with HTTP {response.status_code} "
                    f"(code={error_code})"
                )
            async for line in response.aiter_lines():
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if not data or data == "[DONE]":
                    continue
                try:
                    chunk = json.loads(data)
                except json.JSONDecodeError as error:
                    raise ContractFailure("provider emitted malformed streaming JSON") from error
                if not isinstance(chunk, dict):
                    raise ContractFailure("provider emitted a non-object streaming chunk")
                if isinstance(chunk.get("error"), dict):
                    error_code = chunk["error"].get("code")
                    safe_code = str(error_code) if error_code is not None else "unknown"
                    raise ContractFailure(f"provider emitted a stream error ({safe_code})")
                result.chunks += 1
                if isinstance(chunk.get("usage"), dict):
                    result.usage = chunk["usage"]
                for choice in chunk.get("choices") or []:
                    finish_reason = choice.get("finish_reason")
                    if isinstance(finish_reason, str):
                        result.finish_reasons.append(finish_reason)
                    delta = choice.get("delta") or {}
                    content = delta.get("content")
                    if isinstance(content, str):
                        result.content_parts.append(content)
                    reasoning = delta.get("reasoning_content")
                    if isinstance(reasoning, str):
                        result.reasoning_characters += len(reasoning)
                    for call in delta.get("tool_calls") or []:
                        index = call.get("index", 0)
                        if not isinstance(index, int):
                            raise ContractFailure("provider emitted an invalid tool-call index")
                        function = call.get("function") or {}
                        name = function.get("name")
                        if isinstance(name, str) and name:
                            result.tool_names[index] = name
                        arguments = function.get("arguments")
                        if isinstance(arguments, str):
                            result.tool_arguments.setdefault(index, []).append(arguments)
    except ContractFailure:
        raise
    except httpx.TimeoutException as error:
        raise ContractFailure("provider request timed out") from error
    except httpx.HTTPError as error:
        raise ContractFailure(f"provider transport failed ({type(error).__name__})") from error
    if result.chunks == 0:
        raise ContractFailure("provider returned no streaming chunks")
    return result


async def _check_text(
    client: httpx.AsyncClient,
    *,
    model: str,
    max_tokens: int,
) -> tuple[int, int]:
    result = await _stream_request(
        client,
        payload={
            "model": model,
            "messages": [
                {
                    "role": "system",
                    "content": "你是 Fractal Studio 助手。简洁回答，不调用工具。",
                },
                {"role": "user", "content": "用一句话解释曼德勃罗集边界为何细节丰富。"},
            ],
            "stream": True,
            "stream_options": {"include_usage": True},
            "max_tokens": max_tokens,
            "enable_thinking": False,
            "temperature": 0.1,
            "tools": [TOOL],
            "tool_choice": "auto",
        },
    )
    if not result.content.strip():
        if result.tool_names:
            raise ContractFailure("text request unexpectedly produced only a tool call")
        if result.reasoning_characters:
            raise ContractFailure("text request exhausted its budget on non-visible reasoning")
        finish = ",".join(result.finish_reasons) or "unknown"
        raise ContractFailure(f"text request produced no user-visible content (finish={finish})")
    return result.chunks, _usage_total(result.usage)


async def _check_image_tool(
    client: httpx.AsyncClient,
    *,
    model: str,
    max_tokens: int,
    preview_data_url: str,
) -> tuple[int, int]:
    result = await _stream_request(
        client,
        payload={
            "model": model,
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "你是 Fractal Studio 助手。查看图片后必须调用 propose_studio_patch；"
                        "只建议合法字段，例如 colorMap、iterations、scale。"
                    ),
                },
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "分析这张小预览并建议一项配色调整。"},
                        {"type": "image_url", "image_url": {"url": preview_data_url}},
                    ],
                },
            ],
            "stream": True,
            "stream_options": {"include_usage": True},
            "max_tokens": max_tokens,
            "enable_thinking": False,
            "temperature": 0.1,
            "tools": [TOOL],
            "tool_choice": {
                "type": "function",
                "function": {"name": "propose_studio_patch"},
            },
        },
    )
    matching_indexes = [
        index for index, name in result.tool_names.items() if name == "propose_studio_patch"
    ]
    if not matching_indexes:
        if result.tool_names:
            raise ContractFailure("image request called an unexpected tool")
        if result.content.strip():
            raise ContractFailure("image request returned text instead of the required tool call")
        if result.reasoning_characters:
            raise ContractFailure("image request returned only non-visible reasoning")
        raise ContractFailure("image request did not call propose_studio_patch")
    arguments = "".join(result.tool_arguments.get(matching_indexes[0], []))
    try:
        proposal = json.loads(arguments)
    except json.JSONDecodeError as error:
        raise ContractFailure("tool call arguments were not valid JSON") from error
    if not isinstance(proposal, dict) or not isinstance(proposal.get("patch"), dict):
        raise ContractFailure("tool call did not contain a patch object")
    if not isinstance(proposal.get("reason"), str) or not proposal["reason"].strip():
        raise ContractFailure("tool call did not contain a non-empty reason")
    return result.chunks, _usage_total(result.usage)


async def _run(args: argparse.Namespace) -> None:
    api_key = os.getenv("SILICONFLOW_API_KEY", "").strip()
    if not api_key:
        raise ContractFailure("SILICONFLOW_API_KEY is required in the process environment")
    base_url = os.getenv("SILICONFLOW_BASE_URL", "https://api.siliconflow.cn/v1").rstrip("/")
    if not base_url.startswith("https://"):
        raise ContractFailure("SILICONFLOW_BASE_URL must use HTTPS")
    max_tokens = _bounded_int_env(
        "AI_CONTRACT_MAX_OUTPUT_TOKENS", 256, minimum=64, maximum=1500
    )
    timeout_seconds = _bounded_int_env("AI_CONTRACT_TIMEOUT_SECONDS", 120, minimum=10, maximum=300)
    models = _models(args)
    preview_data_url = _preview_data_url()
    timeout = httpx.Timeout(connect=10, read=timeout_seconds, write=20, pool=10)
    headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}

    async with httpx.AsyncClient(
        base_url=base_url,
        headers=headers,
        timeout=timeout,
        trust_env=False,
    ) as client:
        failures: list[str] = []
        for model in models:
            print(f"CHECK {model}: streaming text", flush=True)
            try:
                text_chunks, text_tokens = await _check_text(
                    client, model=model, max_tokens=max_tokens
                )
                print(
                    f"PASS  {model}: streaming text "
                    f"({text_chunks} chunks, {text_tokens} tokens)",
                    flush=True,
                )
            except ContractFailure as error:
                failures.append(f"{model} text")
                print(f"FAIL  {model}: streaming text: {error}", file=sys.stderr, flush=True)
            print(f"CHECK {model}: in-memory image and forced tool call", flush=True)
            try:
                image_chunks, image_tokens = await _check_image_tool(
                    client,
                    model=model,
                    max_tokens=max_tokens,
                    preview_data_url=preview_data_url,
                )
                print(
                    f"PASS  {model}: image/tool "
                    f"({image_chunks} chunks, {image_tokens} tokens)",
                    flush=True,
                )
            except ContractFailure as error:
                failures.append(f"{model} image/tool")
                print(f"FAIL  {model}: image/tool: {error}", file=sys.stderr, flush=True)
    if failures:
        raise ContractFailure(f"{len(failures)} of {len(models) * 2} checks failed")
    print(f"PASS  SiliconFlow contract ({len(models)} model(s), {len(models) * 2} requests)")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model",
        action="append",
        help="model to check; repeat to compare models (defaults to SILICONFLOW_MODEL)",
    )
    parser.add_argument(
        "--include-candidates",
        action="store_true",
        help="also check all three planned candidate models (six total requests)",
    )
    return parser.parse_args()


def main() -> int:
    try:
        asyncio.run(_run(_parse_args()))
    except ContractFailure as error:
        print(f"FAIL  {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("FAIL  interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
