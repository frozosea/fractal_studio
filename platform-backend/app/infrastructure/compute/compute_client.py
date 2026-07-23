"""Private authenticated Compute HTTP client; no browser-facing transport."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

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


@dataclass(frozen=True, slots=True)
class ComputeArtifact:
    artifact_id: str
    purpose: str
    media_type: str
    size_bytes: int


@dataclass(frozen=True, slots=True)
class ComputeRunStatus:
    run_id: str
    client_job_id: str
    status: str
    progress_percent: int
    artifacts: tuple[ComputeArtifact, ...]
    error_code: str | None = None


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

    async def create_durable_run(self, *, route: str, request_body: dict[str, object]) -> ComputeRunStatus:
        response = await self._request("POST", route, json=request_body)
        return self._run_status(response)

    async def get_run_status(self, *, run_id: str) -> ComputeRunStatus:
        response = await self._request("GET", "/api/runs/status", params={"runId": run_id})
        return self._run_status(response)

    async def cancel_run(self, *, run_id: str) -> None:
        await self._request("POST", "/api/runs/cancel", json={"runId": run_id})

    async def _request(self, method: str, route: str, **kwargs: Any) -> httpx.Response:
        if not self._settings.compute_service_key:
            raise ComputeClientError("compute_not_configured")
        timeout = httpx.Timeout(
            self._settings.compute_read_timeout_seconds,
            connect=self._settings.compute_connect_timeout_seconds,
        )
        try:
            async with httpx.AsyncClient(
                base_url=self._settings.compute_base_url.rstrip("/"),
                timeout=timeout,
                headers={"Authorization": f"Bearer {self._settings.compute_service_key}"},
                trust_env=False,
            ) as client:
                response = await client.request(method, route, **kwargs)
        except httpx.TimeoutException as error:
            raise ComputeClientError("compute_timeout") from error
        except httpx.HTTPError as error:
            raise ComputeClientError("compute_unavailable") from error
        if response.status_code not in {httpx.codes.OK, httpx.codes.ACCEPTED}:
            if response.status_code in {httpx.codes.UNAUTHORIZED, httpx.codes.FORBIDDEN}:
                raise ComputeClientError("compute_auth_failed")
            if response.status_code == httpx.codes.NOT_FOUND:
                raise ComputeClientError("compute_run_not_found")
            if response.status_code == httpx.codes.CONFLICT:
                raise ComputeClientError("compute_conflict")
            if response.status_code >= httpx.codes.INTERNAL_SERVER_ERROR:
                raise ComputeClientError("compute_unavailable")
            raise ComputeClientError("compute_rejected")
        return response

    @staticmethod
    def _run_status(response: httpx.Response) -> ComputeRunStatus:
        try:
            body = response.json()
            artifacts = tuple(
                ComputeArtifact(
                    artifact_id=str(item["artifactId"]),
                    purpose=str(item["purpose"]),
                    media_type=str(item["mediaType"]),
                    size_bytes=int(item["sizeBytes"]),
                )
                for item in body.get("artifacts", [])
            )
            return ComputeRunStatus(
                run_id=str(body["runId"]),
                client_job_id=str(body["clientJobId"]),
                status=str(body["status"]),
                progress_percent=int(body.get("progressPercent", 0)),
                artifacts=artifacts,
                error_code=str(body["error"]) if body.get("error") else None,
            )
        except (KeyError, TypeError, ValueError) as error:
            raise ComputeClientError("compute_invalid_response") from error
