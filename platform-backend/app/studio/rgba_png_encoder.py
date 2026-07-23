"""In-memory RGBA8 Compute response to PNG adapter."""

from __future__ import annotations

from io import BytesIO

from PIL import Image


class InvalidRgbaFrame(ValueError):
    """Compute response cannot safely become browser media."""


def encode_rgba8_png(*, rgba: bytes, width: int, height: int) -> bytes:
    expected_bytes = width * height * 4
    if width <= 0 or height <= 0 or len(rgba) != expected_bytes:
        raise InvalidRgbaFrame("rgba_frame_size_mismatch")
    try:
        image = Image.frombytes("RGBA", (width, height), rgba)
        output = BytesIO()
        image.save(output, format="PNG", optimize=False)
        return output.getvalue()
    except (ValueError, OSError) as error:
        raise InvalidRgbaFrame("rgba_frame_invalid") from error
