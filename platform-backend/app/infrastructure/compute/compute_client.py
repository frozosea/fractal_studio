"""Private authenticated Compute HTTP client; no browser-facing transport."""

from __future__ import annotations

from dataclasses import dataclass

import httpx

from app.core.config import Settings, get_settings


class ComputeClientError(RuntimeError):
    """Stable error code only; caller must never log response bodies or credentials."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


@dataclass(frozen=True, slots=True)
class InlineComputeFrame:
    rgba: bytes
    width: int
    height: int


class ComputeClient:
    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    async def render_map_inline(
        self, request_body: dict[str, object], *, timeout_seconds: float
    ) -> InlineComputeFrame:
        if not self._settings.compute_service_key:
            raise ComputeClientError("compute_not_configured")
        timeout = httpx.Timeout(timeout_seconds, connect=min(timeout_seconds, self._settings.compute_connect_timeout_seconds))
        try:
            async with httpx.AsyncClient(
                base_url=self._settings.compute_base_url.rstrip("/"),
                timeout=timeout,
                headers={"Authorization": f"Bearer {self._settings.compute_service_key}"},
                trust_env=False,
            ) as client:
                response = await client.post("/api/map/render-inline", json=request_body)
        except httpx.TimeoutException as error:
            raise ComputeClientError("compute_timeout") from error
        except httpx.HTTPError as error:
            raise ComputeClientError("compute_unavailable") from error
        if response.status_code != httpx.codes.OK:
            if response.status_code in {httpx.codes.UNAUTHORIZED, httpx.codes.FORBIDDEN}:
                raise ComputeClientError("compute_auth_failed")
            raise ComputeClientError("compute_rejected")
        try:
            width = int(response.headers["X-FSD-Width"])
            height = int(response.headers["X-FSD-Height"])
            pixel_format = response.headers["X-FSD-Pixel-Format"].lower()
        except (KeyError, ValueError) as error:
            raise ComputeClientError("compute_invalid_frame") from error
        if pixel_format != "rgba8":
            raise ComputeClientError("compute_invalid_frame")
        return InlineComputeFrame(rgba=response.content, width=width, height=height)
