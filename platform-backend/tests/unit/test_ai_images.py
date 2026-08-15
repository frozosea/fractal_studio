"""In-memory AI image preparation: bounding, format preservation and safety."""

from __future__ import annotations

from io import BytesIO

import pytest
from PIL import Image

from app.ai.images import MAX_AI_IMAGE_SIDE, prepare_ai_image


def _png(size: tuple[int, int], *, alpha: int = 255) -> bytes:
    output = BytesIO()
    Image.new("RGBA", size, (10, 90, 160, alpha)).save(output, format="PNG")
    return output.getvalue()


def _jpeg(size: tuple[int, int]) -> bytes:
    output = BytesIO()
    Image.new("RGB", size, (10, 90, 160)).save(output, format="JPEG", quality=92)
    return output.getvalue()


def test_large_png_is_scaled_down_and_reencoded_as_jpeg() -> None:
    source = _png((640, 640))
    data, content_type = prepare_ai_image(source, "image/png")
    assert content_type == "image/jpeg"
    with Image.open(BytesIO(data)) as opened:
        assert max(opened.size) == MAX_AI_IMAGE_SIDE
        assert opened.mode == "RGB"
    assert len(data) < len(source)


def test_large_jpeg_is_scaled_down_and_keeps_jpeg() -> None:
    source = _jpeg((800, 600))
    data, content_type = prepare_ai_image(source, "image/jpeg")
    assert content_type == "image/jpeg"
    with Image.open(BytesIO(data)) as opened:
        assert opened.size == (MAX_AI_IMAGE_SIDE, 384)
    assert len(data) < len(source)


def test_transparent_png_is_composited_onto_black() -> None:
    source = _png((640, 640), alpha=0)
    data, content_type = prepare_ai_image(source, "image/png")
    assert content_type == "image/jpeg"
    with Image.open(BytesIO(data)) as opened:
        assert opened.convert("RGB").getpixel((10, 10)) == (0, 0, 0)


def test_small_image_passes_through_unchanged() -> None:
    source = _png((400, 300))
    data, content_type = prepare_ai_image(source, "image/png")
    assert data == source
    assert content_type == "image/png"


def test_exact_bound_image_is_not_upscaled() -> None:
    source = _png((MAX_AI_IMAGE_SIDE, MAX_AI_IMAGE_SIDE))
    data, _ = prepare_ai_image(source, "image/png")
    assert data == source


def test_invalid_bytes_raise_value_error() -> None:
    with pytest.raises(ValueError):
        prepare_ai_image(b"not an image", "image/png")
