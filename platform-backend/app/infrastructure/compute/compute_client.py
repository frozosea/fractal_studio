"""Private authenticated adapter for the current C++ Compute v1 contract."""

from __future__ import annotations

from dataclasses import dataclass
from collections.abc import AsyncIterator
from typing import Any
from urllib.parse import quote

import httpx

from app.core.config import Settings, get_settings
from app.studio.compute_request_mapper import COMPUTE_PREVIEWS_ROUTE


class ComputeClientError(RuntimeError):
    """Stable safe code; raw provider payloads never leave this adapter."""

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
    name: str
    kind: str
    media_type: str
    size_bytes: int
    sha256: str | None = None


@dataclass(frozen=True, slots=True)
class ComputeRunStatus:
    run_id: str
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
        response = await self._request(
            "POST", COMPUTE_PREVIEWS_ROUTE, timeout_seconds=timeout_seconds, json=request_body
        )
        if response.status_code != httpx.codes.OK:
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

    async def create_durable_run(
        self, *, route: str, request_body: dict[str, object]
    ) -> ComputeRunStatus:
        response = await self._request("POST", route, json=request_body)
        return self._run_status(response)

    async def get_run_status(self, *, run_id: str) -> ComputeRunStatus:
        response = await self._request("GET", f"/compute/v1/runs/{quote(run_id, safe='')}")
        return self._run_status(response)

    async def get_run_manifest(self, *, run_id: str) -> ComputeRunStatus:
        response = await self._request("GET", f"/compute/v1/runs/{quote(run_id, safe='')}/manifest")
        return self._manifest_status(response)

    async def cancel_run(self, *, run_id: str) -> None:
        await self._request("POST", f"/compute/v1/runs/{quote(run_id, safe='')}/cancel", json={})

    async def stream_artifact(self, *, artifact_id: str) -> AsyncIterator[bytes]:
        """Read private artifact bytes without exposing a Compute URL to browser code."""
        if not artifact_id or "/" in artifact_id or "\\" in artifact_id or ".." in artifact_id:
            raise ComputeClientError("compute_invalid_artifact_id")
        if not self._settings.compute_service_key:
            raise ComputeClientError("compute_not_configured")
        try:
            async with httpx.AsyncClient(
                base_url=self._settings.compute_base_url.rstrip("/"),
                timeout=httpx.Timeout(self._settings.compute_read_timeout_seconds),
                headers={"Authorization": f"Bearer {self._settings.compute_service_key}"},
                trust_env=False,
            ) as client:
                async with client.stream("GET", "/compute/v1/artifacts", params={"artifactId": artifact_id}) as response:
                    if response.status_code not in {httpx.codes.OK, httpx.codes.PARTIAL_CONTENT}:
                        self._raise_for_status(response)
                    async for chunk in response.aiter_bytes():
                        if chunk:
                            yield chunk
        except ComputeClientError:
            raise
        except httpx.TimeoutException as error:
            raise ComputeClientError("compute_timeout") from error
        except httpx.HTTPError as error:
            raise ComputeClientError("compute_unavailable") from error

    async def _request(
        self, method: str, route: str, *, timeout_seconds: float | None = None, **kwargs: Any
    ) -> httpx.Response:
        if not self._settings.compute_service_key:
            raise ComputeClientError("compute_not_configured")
        timeout = httpx.Timeout(
            timeout_seconds or self._settings.compute_read_timeout_seconds,
            connect=min(
                timeout_seconds or self._settings.compute_read_timeout_seconds,
                self._settings.compute_connect_timeout_seconds,
            ),
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
        if response.status_code in {httpx.codes.OK, httpx.codes.ACCEPTED}:
            return response
        self._raise_for_status(response)
        raise AssertionError("unreachable")

    @staticmethod
    def _raise_for_status(response: httpx.Response) -> None:
        code = ComputeClient._provider_error_code(response)
        if response.status_code in {httpx.codes.UNAUTHORIZED, httpx.codes.FORBIDDEN}:
            raise ComputeClientError("compute_auth_failed")
        if response.status_code == httpx.codes.NOT_FOUND:
            raise ComputeClientError("compute_run_not_found")
        if response.status_code == httpx.codes.CONFLICT:
            raise ComputeClientError("compute_conflict")
        if response.status_code >= httpx.codes.INTERNAL_SERVER_ERROR:
            raise ComputeClientError("compute_unavailable")
        raise ComputeClientError(code)

    @staticmethod
    def _provider_error_code(response: httpx.Response) -> str:
        try:
            error = response.json().get("error", {})
            code = error.get("code") if isinstance(error, dict) else None
            if isinstance(code, str) and code:
                return f"compute_{code.lower()}"
        except (TypeError, ValueError):
            pass
        return "compute_rejected"

    @staticmethod
    def _run_status(response: httpx.Response) -> ComputeRunStatus:
        try:
            body = response.json()
            data = body["data"]
            progress = data.get("progress", {})
            percent = progress.get("percent", 0)
            return ComputeRunStatus(
                run_id=str(data["computeRunId"]),
                status=str(data["status"]),
                progress_percent=max(0, min(100, int(float(percent)))),
                artifacts=ComputeClient._artifacts(data.get("artifacts", [])),
                error_code=ComputeClient._error_from_progress(progress),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise ComputeClientError("compute_invalid_response") from error

    @staticmethod
    def _manifest_status(response: httpx.Response) -> ComputeRunStatus:
        try:
            body = response.json()
            return ComputeRunStatus(
                run_id=str(body["computeRunId"]),
                status=str(body["status"]),
                progress_percent=100 if body["status"] == "completed" else 0,
                artifacts=ComputeClient._artifacts(body.get("artifacts", [])),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise ComputeClientError("compute_invalid_manifest") from error

    @staticmethod
    def _artifacts(items: object) -> tuple[ComputeArtifact, ...]:
        if not isinstance(items, list):
            raise ValueError("artifacts must be an array")
        return tuple(
            ComputeArtifact(
                artifact_id=str(item["artifactId"]),
                name=str(item.get("name", "")),
                kind=str(item.get("kind", "")),
                media_type=str(item.get("mediaType", "")),
                size_bytes=int(item.get("sizeBytes", 0)),
                sha256=str(item["sha256"]) if item.get("sha256") else None,
            )
            for item in items
            if isinstance(item, dict)
        )

    @staticmethod
    def _error_from_progress(progress: object) -> str | None:
        if not isinstance(progress, dict):
            return None
        value = progress.get("errorCode") or progress.get("error")
        return str(value) if isinstance(value, str) and value else None
