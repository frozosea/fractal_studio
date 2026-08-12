#!/usr/bin/env python3
"""Explicit paid contract for the production SiliconFlow runtime adapter.

This script is never imported by pytest or application startup. It calls
``app.ai.provider.stream_completion`` directly, uses a caller-supplied real
preview and trusted Studio context, and never prints prompts, model output,
paths, context, provider error bodies or credentials.
"""

from __future__ import annotations

import argparse
import asyncio
from contextlib import contextmanager
import io
import json
import os
from pathlib import Path
import secrets
import socket
import sys
import time
from typing import Iterator

from PIL import Image, UnidentifiedImageError

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from app.ai.models import validate_studio_suggestion  # noqa: E402
from app.ai.provider import ProviderUnavailable, stream_completion  # noqa: E402
from app.core.config import get_settings, reveal_secret  # noqa: E402


class ContractFailure(RuntimeError):
    """A sanitized failure which is safe to render in a development terminal."""


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number: {value}")


def _load_context(path: Path) -> dict[str, object]:
    if not path.is_file() or path.stat().st_size > 100_000:
        raise ContractFailure("context must be an existing JSON file no larger than 100 KiB")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            parse_constant=_reject_json_constant,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise ContractFailure("context is not valid UTF-8 JSON") from error
    required = {"spec", "mode", "output", "capabilities", "member"}
    if not isinstance(value, dict) or not required <= set(value):
        raise ContractFailure(
            "trusted context must contain spec, mode, output, capabilities and member"
        )
    if not isinstance(value["capabilities"], dict) or not isinstance(value["member"], bool):
        raise ContractFailure("trusted context has invalid capabilities or member fields")
    return value


def _load_preview(path: Path, *, max_bytes: int) -> tuple[bytes, str]:
    if not path.is_file():
        raise ContractFailure("preview file does not exist")
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ContractFailure("preview could not be read") from error
    hard_limit = min(max_bytes, 1_048_576)
    if not 1 <= len(data) <= hard_limit:
        raise ContractFailure("preview must be non-empty and no larger than the AI image limit")
    try:
        with Image.open(io.BytesIO(data)) as image:
            if max(image.size) > 640:
                raise ContractFailure("preview longest edge must be at most 640 px")
            image_type = Image.MIME.get(image.format or "")
            image.verify()
    except (
        Image.DecompressionBombError,
        OSError,
        UnidentifiedImageError,
        ValueError,
    ) as error:
        raise ContractFailure("preview is not a valid bounded image") from error
    if image_type not in {"image/png", "image/jpeg", "image/webp"}:
        raise ContractFailure("preview must be PNG, JPEG or WebP")
    return data, image_type


def _usage_tokens(usage: object) -> int:
    if not isinstance(usage, dict):
        raise ContractFailure("provider stream omitted token usage")
    value = usage.get("total_tokens")
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ContractFailure("provider returned invalid token usage")
    return value


def _sanitized_provider_failure(stage: str, error: ProviderUnavailable) -> ContractFailure:
    return ContractFailure(
        f"{stage} failed through ProviderUnavailable (retryable={error.retryable})"
    )


async def _check_real_text(
    *, text: str, context: dict[str, object]
) -> tuple[int, int, int]:
    delta_count = 0
    character_count = 0
    usage: object = None
    suggestions = 0
    try:
        async for event, payload in stream_completion(
            text=text,
            history=[],
            context=context,
            image=None,
            image_type=None,
            assistant_mode="chat",
        ):
            if event == "delta" and isinstance(payload, str) and payload:
                delta_count += 1
                character_count += len(payload)
            elif event == "usage":
                usage = payload
            elif event == "suggestion":
                suggestions += 1
    except ProviderUnavailable as error:
        raise _sanitized_provider_failure("real text stream", error) from None
    if delta_count < 1 or character_count < 1:
        raise ContractFailure("real text stream returned no visible delta")
    if suggestions:
        raise ContractFailure("knowledge text unexpectedly returned a Studio tool call")
    return delta_count, character_count, _usage_tokens(usage)


async def _check_real_image_tool(
    *, context: dict[str, object], image: bytes, image_type: str
) -> tuple[int, int, int]:
    suggestion_count = 0
    visible_characters = 0
    usage: object = None
    checked_suggestion: dict[str, object] | None = None
    try:
        async for event, payload in stream_completion(
            text=(
                "分析这张实际 Studio 预览，只调用 propose_studio_patch 提出一项最小、合法的"
                "配色调整。理由只能依据图片中实际可见的颜色与明暗层次，不要声称已经应用。"
            ),
            history=[],
            context=context,
            image=image,
            image_type=image_type,
            force_patch=True,
            assistant_mode="chat",
        ):
            if event == "delta" and isinstance(payload, str):
                visible_characters += len(payload)
            elif event == "usage":
                usage = payload
            elif event == "suggestion":
                suggestion_count += 1
                checked_suggestion = validate_studio_suggestion(payload, context)
    except ProviderUnavailable as error:
        raise _sanitized_provider_failure("real image/tool stream", error) from None
    if suggestion_count != 1 or checked_suggestion is None:
        raise ContractFailure("real image stream did not return one validated Studio tool call")
    return suggestion_count, visible_characters, _usage_tokens(usage)


async def _check_stop_after_first_delta(*, context: dict[str, object]) -> None:
    iterator = stream_completion(
        text=(
            "用三句简短文字解释逃逸时间算法。只输出普通文字，不调用工具；"
            "这个请求会在首个可见分片后由合同测试主动停止。"
        ),
        history=[],
        context=context,
        image=None,
        image_type=None,
        disable_tools=True,
        assistant_mode="chat",
    )
    closed = False
    try:
        while True:
            try:
                event, payload = await asyncio.wait_for(anext(iterator), timeout=120)
            except StopAsyncIteration as error:
                raise ContractFailure("stop check ended before the first visible delta") from error
            if event != "delta" or not isinstance(payload, str) or not payload:
                continue
            await asyncio.wait_for(iterator.aclose(), timeout=10)
            closed = True
            break
        try:
            await anext(iterator)
        except StopAsyncIteration:
            return
        raise ContractFailure("adapter yielded another event after explicit aclose")
    except ProviderUnavailable as error:
        raise _sanitized_provider_failure("stop-after-first-delta stream", error) from None
    except TimeoutError as error:
        raise ContractFailure("stop-after-first-delta stream timed out") from error
    finally:
        if not closed:
            await iterator.aclose()


@contextmanager
def _temporary_environment(**values: str) -> Iterator[None]:
    previous = {key: os.environ.get(key) for key in values}
    os.environ.update(values)
    get_settings.cache_clear()
    try:
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        get_settings.cache_clear()


def _error_rendering(error: ProviderUnavailable) -> str:
    return "\n".join(
        (
            str(error),
            repr(error),
            repr(error.__cause__),
            repr(error.__context__),
        )
    )


async def _check_controlled_connection_failure(*, secret: str) -> None:
    # Keeping a loopback port bound but not listening reserves a deterministic
    # closed target without starting a mock provider or racing another process.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as guard:
        guard.bind(("127.0.0.1", 0))
        port = int(guard.getsockname()[1])
        with _temporary_environment(
            SILICONFLOW_BASE_URL=f"http://127.0.0.1:{port}/v1"
        ):
            try:
                events = [
                    event
                    async for event in stream_completion(
                        text="connection failure contract",
                        history=[],
                        context={},
                        image=None,
                        image_type=None,
                        disable_tools=True,
                    )
                ]
            except ProviderUnavailable as error:
                if not error.retryable:
                    raise ContractFailure(
                        "controlled connection failure was not marked retryable"
                    ) from None
                if secret and secret in _error_rendering(error):
                    raise ContractFailure("controlled connection failure leaked the provider key")
                return
            if events:
                raise ContractFailure("closed loopback port unexpectedly yielded provider events")
            raise ContractFailure("closed loopback port did not map to ProviderUnavailable")


async def _check_invalid_authentication(*, real_secret: str) -> None:
    invalid_secret = "sf-invalid-contract-" + secrets.token_urlsafe(24)
    with _temporary_environment(SILICONFLOW_API_KEY=invalid_secret):
        try:
            events = [
                event
                async for event in stream_completion(
                    text="invalid authentication contract",
                    history=[],
                    context={},
                    image=None,
                    image_type=None,
                    disable_tools=True,
                )
            ]
        except ProviderUnavailable as error:
            rendered = _error_rendering(error)
            if invalid_secret in rendered or (real_secret and real_secret in rendered):
                raise ContractFailure("invalid-auth failure leaked a provider key") from None
            if error.retryable:
                raise ContractFailure("invalid authentication was incorrectly marked retryable")
            return
        if events:
            raise ContractFailure("invalid authentication unexpectedly yielded provider events")
        raise ContractFailure("invalid authentication did not map to ProviderUnavailable")


async def _run(args: argparse.Namespace) -> None:
    if not args.confirm_paid:
        raise ContractFailure("pass --confirm-paid to run explicit real provider requests")
    settings = get_settings()
    secret = reveal_secret(settings.siliconflow_api_key)
    if not settings.ai_enabled or not secret:
        raise ContractFailure("AI must be enabled with a non-empty provider key")
    if not settings.siliconflow_base_url.startswith("https://"):
        raise ContractFailure("the real provider base URL must use HTTPS")
    context = _load_context(args.context)
    image, image_type = _load_preview(
        args.preview,
        max_bytes=settings.ai_max_image_bytes,
    )

    started = time.monotonic()
    await _check_controlled_connection_failure(secret=secret)
    text_deltas, text_characters, text_tokens = await _check_real_text(
        text=args.text,
        context=context,
    )
    tool_calls, tool_characters, tool_tokens = await _check_real_image_tool(
        context=context,
        image=image,
        image_type=image_type,
    )
    await _check_stop_after_first_delta(context=context)
    if args.check_invalid_auth:
        await _check_invalid_authentication(real_secret=secret)
    elapsed = time.monotonic() - started

    auth_check = "passed" if args.check_invalid_auth else "skipped"
    print(
        "PASS runtime-adapter "
        f"model={settings.siliconflow_model} "
        f"text={text_deltas}d/{text_characters}c/{text_tokens}t "
        f"image_tool={tool_calls}/{tool_characters}c/{tool_tokens}t "
        f"stop=closed connection=mapped invalid_auth={auth_check} "
        f"preview_bytes={len(image)} elapsed={elapsed:.2f}s"
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", type=Path, required=True, help="actual bounded Studio preview")
    parser.add_argument("--context", type=Path, required=True, help="trusted Studio context JSON")
    parser.add_argument(
        "--text",
        default="用两句话解释逃逸时间分形为什么在边界附近出现丰富细节；只解释概念，不修改参数。",
        help="real text-stream prompt; content is never printed",
    )
    parser.add_argument(
        "--confirm-paid",
        action="store_true",
        help="required acknowledgement that this makes real paid provider requests",
    )
    parser.add_argument(
        "--check-invalid-auth",
        action="store_true",
        help="also make one real request with a temporary invalid credential",
    )
    return parser.parse_args()


def main() -> int:
    try:
        asyncio.run(_run(_parse_args()))
    except ContractFailure as error:
        print(f"FAIL runtime-adapter contract: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("FAIL runtime-adapter contract: interrupted", file=sys.stderr)
        return 130
    except Exception as error:
        # Unexpected third-party exceptions may embed request diagnostics. Only
        # print the exception type; the paid script must never leak their text.
        print(
            f"FAIL runtime-adapter contract: unexpected {type(error).__name__}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
