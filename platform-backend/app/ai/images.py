"""In-memory image preparation for AI provider calls.

Provider vision tokens scale with pixel count, and every exploration call
re-encodes the same preview.  Bounding the longest side and re-encoding as
JPEG before upload cuts both input cost and upload size without touching the
original asset bytes.  JPEG is the same choice the listing-copy pipeline makes
for visual analysis: structural detail survives, and the encoding is
predictable under hard size limits.
"""

from __future__ import annotations

from io import BytesIO

from PIL import Image, UnidentifiedImageError

# Bound for Studio preview uploads.  512 keeps structural detail (centre
# shape, band directions, colour regions) while roughly halving provider
# image tokens compared with the previous 640 maximum.
MAX_AI_IMAGE_SIDE = 512

_JPEG_QUALITY = 90


def prepare_ai_image(data: bytes, content_type: str) -> tuple[bytes, str]:
    """Scale (never upscale) an image in memory, re-encoding as JPEG.

    Images whose longest side already fits the bound are returned unchanged so
    that verified uploads are not re-encoded unnecessarily.  Larger uploads are
    downscaled, composited onto the black Studio preview background when they
    carry transparency, and returned as JPEG.
    """
    try:
        with Image.open(BytesIO(data)) as opened:
            if max(opened.size) <= MAX_AI_IMAGE_SIDE:
                return data, content_type
            opened.thumbnail((MAX_AI_IMAGE_SIDE, MAX_AI_IMAGE_SIDE), Image.Resampling.LANCZOS)
            if opened.mode in ("RGBA", "LA"):
                alpha = opened.getchannel("A")
                if alpha.getextrema() != (255, 255):
                    background = Image.new("RGBA", opened.size, (0, 0, 0, 255))
                    opened = Image.alpha_composite(background, opened.convert("RGBA"))
            output = BytesIO()
            opened.convert("RGB").save(output, format="JPEG", quality=_JPEG_QUALITY, optimize=True)
            return output.getvalue(), "image/jpeg"
    except (UnidentifiedImageError, OSError, ValueError, Image.DecompressionBombError) as error:
        # The upload was verified before this point; treat a re-encode failure
        # (including a decompression bomb whose header lies about its size) as
        # an invalid image rather than forwarding raw bytes.
        raise ValueError("AI image could not be re-encoded") from error
