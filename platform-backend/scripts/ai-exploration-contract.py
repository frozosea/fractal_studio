#!/usr/bin/env python3
"""Run the real Studio exploration contract against an actual Compute preview.

This is deliberately opt-in and paid. It never substitutes a generated fixture
for model quality: callers supply the exact preview and trusted Studio context
used for acceptance. Credentials are read only through application settings and
are never printed.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import sys

from PIL import Image, UnidentifiedImageError

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from app.ai.exploration import ExplorationMode, validate_candidate_set  # noqa: E402
from app.ai.provider import ProviderUnavailable, stream_completion  # noqa: E402


class ContractFailure(RuntimeError):
    """Sanitized failure safe to show in a development log."""


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number: {value}")


MODE_INSTRUCTIONS: dict[ExplorationMode, str] = {
    "location": (
        "基于上面的图像观察，只调用 propose_studio_patch 给出三个位置探索候选。"
        "当前基准会单独显示，三个候选都必须有非零且明显不同的变化。"
        "严格选择 position 或 scale 单一变量轴；只返回归一化相对位移/缩放，不计算绝对坐标。"
    ),
    "color": (
        "基于上面的图像观察，只调用 propose_studio_patch 给出四个视觉上明显不同的调色候选。"
        "只修改调色字段，并准确说明它会如何改变图片中实际可见的层次。"
    ),
    "composition": (
        "基于上面的图像观察，只调用 propose_studio_patch 给出三个构图候选。"
        "当前基准会单独显示，三个候选都必须有非零且明显不同的变化。"
        "scaleFactor<1 表示放大收紧，>1 表示缩小视图显示更多周边。"
        "只返回归一化相对平移、缩放和旋转，不计算绝对坐标，不改颜色或公式。"
    ),
}


def _load_context(path: Path) -> dict[str, object]:
    if not path.is_file() or path.stat().st_size > 100_000:
        raise ContractFailure("context must be an existing JSON file no larger than 100 KiB")
    try:
        value = json.loads(path.read_text(encoding="utf-8"), parse_constant=_reject_json_constant)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise ContractFailure("context is not valid UTF-8 JSON") from error
    if not isinstance(value, dict) or not all(key in value for key in ("spec", "output", "capabilities")):
        raise ContractFailure("context must contain spec, output and trusted capabilities")
    return value


def _load_preview(path: Path) -> tuple[bytes, str]:
    if not path.is_file():
        raise ContractFailure("preview file does not exist")
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ContractFailure("preview could not be read") from error
    if not 1 <= len(data) <= 1_048_576:
        raise ContractFailure("preview must be between 1 byte and 1 MiB")
    try:
        with Image.open(path) as image:
            image.verify()
        with Image.open(path) as image:
            if max(image.size) > 640:
                raise ContractFailure("preview longest edge must be at most 640 px")
            image_type = Image.MIME.get(image.format or "")
    except (OSError, UnidentifiedImageError) as error:
        raise ContractFailure("preview is not a valid image") from error
    if image_type not in {"image/png", "image/jpeg", "image/webp"}:
        raise ContractFailure("preview must be PNG, JPEG or WebP")
    return data, image_type


def _usage_tokens(usage: object) -> int:
    if not isinstance(usage, dict):
        raise ContractFailure("provider stream omitted token usage")
    value = usage.get("total_tokens")
    if not isinstance(value, int) or value <= 0:
        raise ContractFailure("provider returned invalid token usage")
    return value


async def _visual_observation(
    *, context: dict[str, object], image: bytes, image_type: str, mode: ExplorationMode
) -> tuple[str, int]:
    parts: list[str] = []
    usage: object = None
    async for event, payload in stream_completion(
        text=(
            "只根据附图，按中心、左上、右上、左下、右下、整体色彩六项各写一句短观察。"
            "明确亮细节、大片暗区、主体相对位置和边缘裁切；看不清就写不确定。"
            "禁止比喻、提出修改、输出参数、分形名称或任何数学身份。"
        ),
        history=[],
        context=context,
        image=image,
        image_type=image_type,
        disable_tools=True,
        assistant_mode=mode,
    ):
        if event == "delta":
            parts.append(str(payload))
        elif event == "usage":
            usage = payload
    observation = "".join(parts).strip()
    if len(observation) < 40:
        raise ContractFailure(f"{mode} visual phase returned too little visible analysis")
    return observation, _usage_tokens(usage)


async def _candidate_call(
    *, context: dict[str, object], mode: ExplorationMode, observation: str,
    show_details: bool,
) -> tuple[dict[str, object], int]:
    raw: object = None
    usage: object = None
    visible_text: list[str] = []
    async for event, payload in stream_completion(
        text=MODE_INSTRUCTIONS[mode],
        history=[
            {"role": "user", "content": "请按当前作品完成这项探索。"},
            {"role": "assistant", "content": observation},
        ],
        context=context,
        # The visual phase already consumed the exact preview. Re-uploading the
        # same 500 KiB image for the tool phase materially increases provider
        # queue time without adding evidence; the observation is the hand-off.
        image=None,
        image_type=None,
        force_patch=True,
        assistant_mode=mode,
    ):
        if event == "suggestion":
            raw = payload
        elif event == "usage":
            usage = payload
        elif event == "delta":
            visible_text.append(str(payload))
    if visible_text:
        raise ContractFailure(f"{mode} candidate phase returned prose instead of only the forced tool")
    checked = validate_candidate_set(raw, context, mode)
    if checked is None:
        if show_details:
            print(f"INVALID {mode}: {json.dumps(raw, ensure_ascii=False, sort_keys=True)}")
        raise ContractFailure(f"{mode} tool arguments failed the real Platform validator")
    expected = 4 if mode == "color" else 3
    candidates = checked.get("candidates")
    if not isinstance(candidates, list) or len(candidates) != expected:
        raise ContractFailure(f"{mode} returned the wrong candidate count")
    return checked, _usage_tokens(usage)


async def _run(args: argparse.Namespace) -> None:
    context = _load_context(args.context)
    image, image_type = _load_preview(args.preview)
    modes: tuple[ExplorationMode, ...] = tuple(args.mode or ("location", "color", "composition"))
    failures: list[str] = []
    for attempt in range(1, args.attempts + 1):
        for mode in modes:
            print(f"CHECK {mode} attempt {attempt}: real image analysis + forced candidates", flush=True)
            try:
                observation, analysis_tokens = await _visual_observation(
                    context=context, image=image, image_type=image_type, mode=mode
                )
                result, candidate_tokens = await _candidate_call(
                    context=context,
                    mode=mode,
                    observation=observation,
                    show_details=args.show_details,
                )
                print(
                    f"PASS  {mode} attempt {attempt}: {len(result['candidates'])} validated candidates "
                    f"({analysis_tokens + candidate_tokens} tokens)",
                    flush=True,
                )
                if args.show_details:
                    print(f"OBSERVATION {mode}: {observation}")
                    for candidate in result["candidates"]:
                        print(
                            f"CANDIDATE {mode}/{candidate['id']}: {candidate['label']} | "
                            f"{json.dumps(candidate['patch'], ensure_ascii=False, sort_keys=True)} | "
                            f"{candidate['reason']}"
                        )
            except (ContractFailure, ProviderUnavailable) as error:
                failures.append(f"{mode}#{attempt}")
                print(f"FAIL  {mode} attempt {attempt}: {error}", flush=True)
    if failures:
        raise ContractFailure(f"{len(failures)} exploration checks failed: {', '.join(failures)}")
    print(f"PASS  real Studio exploration contract ({len(modes) * args.attempts} checks)")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", type=Path, required=True, help="actual <=640 px Studio preview")
    parser.add_argument("--context", type=Path, required=True, help="trusted Studio context JSON")
    parser.add_argument(
        "--mode",
        action="append",
        choices=("location", "color", "composition"),
        help="mode to check; repeat as needed (default: all three)",
    )
    parser.add_argument("--attempts", type=int, choices=range(1, 6), default=1)
    parser.add_argument("--show-details", action="store_true")
    return parser.parse_args()


def main() -> None:
    try:
        asyncio.run(_run(_parse_args()))
    except ContractFailure as error:
        print(f"CONTRACT FAILED: {error}")
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
