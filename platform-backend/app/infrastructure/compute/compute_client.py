from __future__ import annotations

import asyncio
import hashlib
from typing import Any

import httpx

from app.core.config import Settings


class ComputeError(RuntimeError):
    def __init__(self, code: str, message: str, status_code: int = 502, details: dict[str, Any] | None = None):
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.details = details or {}


class ComputeClient:
    def __init__(self, settings: Settings, client: httpx.AsyncClient | None = None) -> None:
        if not settings.compute_service_key:
            raise RuntimeError("COMPUTE_SERVICE_KEY is required")
        self._base_url = settings.compute_base_url
        self._headers = {"Authorization": f"Bearer {settings.compute_service_key}"}
        self._client = client or httpx.AsyncClient(
            timeout=httpx.Timeout(connect=5, read=600, write=30, pool=5),
            headers=self._headers,
        )
        self._owns_client = client is None

    async def close(self) -> None:
        if self._owns_client:
            await self._client.aclose()

    async def _request(self, method: str, path: str, **kwargs: Any) -> httpx.Response:
        for attempt in range(3):
            try:
                response = await self._client.request(method, f"{self._base_url}{path}", **kwargs)
            except (httpx.ConnectError, httpx.ConnectTimeout, httpx.ReadTimeout) as exc:
                if attempt == 2:
                    raise ComputeError("COMPUTE_UNAVAILABLE", str(exc), 503) from exc
                await asyncio.sleep(0.1 * (2**attempt))
                continue
            if response.status_code in {502, 503, 504} and attempt < 2:
                await asyncio.sleep(0.1 * (2**attempt))
                continue
            if response.is_error:
                try:
                    error = response.json().get("error", {})
                except (AttributeError, ValueError):
                    error = {}
                if isinstance(error, str):
                    error = {"message": error}
                elif not isinstance(error, dict):
                    error = {}
                raise ComputeError(
                    str(error.get("code", "COMPUTE_REQUEST_FAILED")),
                    str(error.get("message", response.text or "Compute request failed")),
                    response.status_code,
                    error.get("details") if isinstance(error.get("details"), dict) else {},
                )
            return response
        raise AssertionError("unreachable")

    async def capabilities(self) -> dict[str, Any]:
        return (await self._request("GET", "/compute/v1/capabilities")).json()

    async def preview(self, kind: str, payload: dict[str, Any], request_id: str) -> httpx.Response:
        return await self._request(
            "POST", "/compute/v1/previews",
            headers={**self._headers, "X-Request-Id": request_id},
            json={"schemaVersion": 1, "kind": kind, "payload": payload},
        )

    async def create_run(
        self, kind: str, payload: dict[str, Any], platform_job_id: str, request_id: str
    ) -> dict[str, Any]:
        response = await self._request(
            "POST", "/compute/v1/runs",
            headers={**self._headers, "X-Request-Id": request_id},
            json={
                "schemaVersion": 1,
                "kind": kind,
                "idempotencyKey": platform_job_id,
                "payload": payload,
            },
        )
        return response.json()["data"]

    async def get_run(self, compute_run_id: str) -> dict[str, Any]:
        return (await self._request("GET", f"/compute/v1/runs/{compute_run_id}")).json()["data"]

    async def cancel_run(self, compute_run_id: str) -> dict[str, Any]:
        return (
            await self._request("POST", f"/compute/v1/runs/{compute_run_id}/cancel", json={})
        ).json()["data"]

    async def manifest(self, compute_run_id: str) -> dict[str, Any]:
        return (await self._request("GET", f"/compute/v1/runs/{compute_run_id}/manifest")).json()

    async def verify_artifact(
        self, artifact_id: str, expected_size: int, expected_sha256: str,
    ) -> None:
        digest = hashlib.sha256()
        size = 0
        try:
            async with self._client.stream(
                "GET",
                f"{self._base_url}/compute/v1/artifacts",
                headers=self._headers,
                params={"artifactId": artifact_id},
            ) as response:
                if response.is_error:
                    await response.aread()
                    try:
                        error = response.json().get("error", {})
                    except (AttributeError, ValueError):
                        error = {}
                    if isinstance(error, str):
                        error = {"message": error}
                    elif not isinstance(error, dict):
                        error = {}
                    raise ComputeError(
                        str(error.get("code", "COMPUTE_REQUEST_FAILED")),
                        str(error.get("message", response.text or "Compute request failed")),
                        response.status_code,
                        error.get("details") if isinstance(error.get("details"), dict) else {},
                    )
                async for chunk in response.aiter_bytes():
                    digest.update(chunk)
                    size += len(chunk)
        except (httpx.ConnectError, httpx.ConnectTimeout, httpx.ReadTimeout) as exc:
            raise ComputeError("COMPUTE_UNAVAILABLE", str(exc), 503) from exc
        actual_sha256 = digest.hexdigest()
        if size != expected_size or actual_sha256.lower() != expected_sha256.lower():
            raise ComputeError(
                "ARTIFACT_INTEGRITY_FAILED",
                "Compute artifact does not match its manifest",
                502,
                {
                    "artifactId": artifact_id,
                    "expectedSizeBytes": expected_size,
                    "actualSizeBytes": size,
                    "expectedSha256": expected_sha256,
                    "actualSha256": actual_sha256,
                },
            )
