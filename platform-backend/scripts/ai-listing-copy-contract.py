#!/usr/bin/env python3
"""Explicit real-model listing-copy check using an actual rendered artwork image.

No synthetic or mock preview is generated. Supply a real final/derivative image
through ``--image`` (or ``AI_LISTING_CONTRACT_IMAGE``). Credentials and prompts
are never printed. Use ``--show-candidates`` only when intentionally reviewing
the generated copy in a trusted terminal.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.ai.listing_provider import ListingProviderUnavailable, generate_listing_copy
from app.ai.listing_service import prepare_listing_preview
from app.ai.provider_config import uses_deepseek
from app.core.config import get_settings


def _context(path: Path | None) -> dict[str, object]:
    if path is None:
        return {
            "existingTitle": "",
            "existingDescription": "",
            "existingTags": [],
            "render": {},
        }
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError("context JSON must contain an object")
    return value


async def _generate_with_release_policy(**kwargs):
    """Mirror the API's one retry before any user-visible output exists."""

    for attempt in range(2):
        try:
            return await generate_listing_copy(**kwargs)
        except ListingProviderUnavailable as error:
            if attempt == 1 or not error.retryable:
                raise
    raise RuntimeError("provider returned no completion")


async def _run(args: argparse.Namespace) -> None:
    configured = get_settings()
    provider_name = "deepseek" if uses_deepseek(configured) else "siliconflow"
    model = configured.deepseek_model if uses_deepseek(configured) else configured.siliconflow_model
    image_path = Path(args.image or os.getenv("AI_LISTING_CONTRACT_IMAGE", ""))
    if not image_path.is_file():
        raise RuntimeError("--image must point to an actual rendered artwork image")
    source = image_path.read_bytes()
    image, image_type = prepare_listing_preview(source)
    started = time.monotonic()
    listing_context = _context(Path(args.context_json) if args.context_json else None)
    completion = await _generate_with_release_policy(
        locale=args.locale,
        listing_context=listing_context,
        image=image,
        image_type=image_type,
        settings=configured,  # type: ignore[arg-type]
    )
    completions = [completion]
    if args.revision_instruction:
        completion = await _generate_with_release_policy(
            locale=args.locale,
            listing_context=listing_context,
            image=image,
            image_type=image_type,
            prior_candidates=[
                candidate.model_dump(mode="json") for candidate in completion.candidates
            ],
            instruction=args.revision_instruction,
            settings=configured,  # type: ignore[arg-type]
        )
        completions.append(completion)
    elapsed = time.monotonic() - started
    total_tokens = 0
    for result in completions:
        usage = result.usage or {}
        request_tokens = usage.get("total_tokens")
        if not isinstance(request_tokens, int) or request_tokens <= 0:
            raise RuntimeError("provider response did not include valid token usage")
        total_tokens += request_tokens
    if args.show_candidates:
        print(
            json.dumps(
                [candidate.model_dump(mode="json") for candidate in completion.candidates],
                ensure_ascii=False,
                indent=2,
            )
        )
    print(
        f"PASS listing-copy {provider_name}/{model}: 3 strict candidates, "
        f"{len(completions)} request(s), {total_tokens} tokens, {elapsed:.2f}s, "
        f"image={len(image)} bytes"
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", help="actual final or derivative artwork image")
    parser.add_argument("--locale", choices=("zh", "en"), default="zh")
    parser.add_argument("--context-json", help="optional server-safe listing context JSON")
    parser.add_argument(
        "--revision-instruction",
        help="also make a second real request revising the first three candidates",
    )
    parser.add_argument("--show-candidates", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    try:
        asyncio.run(_run(_parse_args()))
    except Exception as error:
        raise SystemExit(f"FAIL listing-copy contract: {error}") from None
