"""Bounded non-persistent synchronous preview."""

from __future__ import annotations

from uuid import UUID, uuid4

from fastapi import HTTPException, status

from app.core.config import Settings, get_settings
from app.infrastructure.compute.compute_client import ComputeClient, ComputeClientError
from app.infrastructure.redis.quota_store import PreviewQuotaUnavailable
from app.studio.compute_request_mapper import map_preview_v1
from app.studio.quota_service import PreviewRateLimiter
from app.studio.recipe_service import CanonicalRecipe
from app.studio.rgba_png_encoder import InvalidRgbaFrame, encode_rgba8_png


class PreviewService:
    def __init__(
        self,
        *,
        settings: Settings | None = None,
        rate_limiter: PreviewRateLimiter | None = None,
        compute_client: ComputeClient | None = None,
    ) -> None:
        self._settings = settings or get_settings()
        self._rate_limiter = rate_limiter or PreviewRateLimiter()
        self._compute_client = compute_client or ComputeClient(self._settings)

    async def render(
        self, *, owner_id: UUID, canonical: CanonicalRecipe, width: int, height: int, request_id: UUID | None = None
    ) -> bytes:
        self._validate_dimensions(width, height)
        try:
            allowed = await self._rate_limiter.allow(owner_id)
        except PreviewQuotaUnavailable as error:
            raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="preview_rate_limit_unavailable") from error
        if not allowed:
            raise HTTPException(status_code=status.HTTP_429_TOO_MANY_REQUESTS, detail="preview_rate_limited")
        compute_request = map_preview_v1(
            canonical.spec, width=width, height=height, request_id=request_id or uuid4()
        )
        try:
            frame = await self._compute_client.render_map_inline(
                compute_request, timeout_seconds=5.0
            )
            if frame.width != width or frame.height != height:
                raise InvalidRgbaFrame("compute_frame_dimensions_mismatch")
            return encode_rgba8_png(rgba=frame.rgba, width=width, height=height)
        except ComputeClientError as error:
            raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=error.code) from error
        except InvalidRgbaFrame as error:
            raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail="compute_invalid_frame") from error

    def _validate_dimensions(self, width: int, height: int) -> None:
        if (
            width > self._settings.preview_max_width
            or height > self._settings.preview_max_height
            or width * height > self._settings.preview_max_pixels
        ):
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="preview_dimensions_exceeded")
