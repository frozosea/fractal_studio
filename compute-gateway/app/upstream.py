"""Authenticated HTTP adapter for one C++ Compute node."""

from __future__ import annotations

import json
from collections.abc import AsyncIterator
from dataclasses import dataclass
from time import monotonic
from urllib.parse import quote

import httpx

from app.config import Settings, get_settings
from app.models import ComputeNode

# A misbehaving or compromised node must not be able to OOM the gateway by
# streaming an unbounded body (production mem_limit is 384m). JSON and preview
# responses are buffered, so they get hard byte budgets; artifacts stream.
MAX_JSON_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_PREVIEW_RESPONSE_BYTES = 64 * 1024 * 1024


class UpstreamError(RuntimeError):
    def __init__(self, code: str, *, status_code: int | None = None) -> None:
        super().__init__(code)
        self.code = code
        self.status_code = status_code


@dataclass(frozen=True, slots=True)
class UpstreamReply:
    status_code: int
    body: dict[str, object]
    headers: dict[str, str]


class ComputeNodeClient:
    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    def _headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self._settings.compute_upstream_service_key}"}

    @staticmethod
    def _url(node: ComputeNode, route: str) -> str:
        return f"{node.base_url.rstrip('/')}{route}"

    async def health(self, node: ComputeNode) -> int:
        started = monotonic()
        try:
            async with httpx.AsyncClient(
                timeout=httpx.Timeout(self._settings.create_connect_timeout_seconds), trust_env=False
            ) as client:
                response = await client.get(self._url(node, "/compute/v1/health"))
        except httpx.HTTPError as error:
            raise UpstreamError("COMPUTE_NODE_UNAVAILABLE") from error
        if response.status_code != 200:
            raise UpstreamError("COMPUTE_NODE_UNAVAILABLE", status_code=response.status_code)
        return int((monotonic() - started) * 1000)

    async def capabilities(self, node: ComputeNode) -> dict[str, object]:
        reply = await self._json(node, "GET", "/compute/v1/capabilities", expected={200})
        return reply.body

    async def preview(self, node: ComputeNode, envelope: dict[str, object]) -> tuple[bytes, str, dict[str, str]]:
        try:
            async with (
                httpx.AsyncClient(
                    timeout=httpx.Timeout(self._settings.preview_timeout_seconds), trust_env=False
                ) as client,
                client.stream(
                    "POST", self._url(node, "/compute/v1/previews"), headers=self._headers(), json=envelope
                ) as response,
            ):
                if response.status_code != 200:
                    self._raise_response(response)
                body = await self._read_bounded(response, MAX_PREVIEW_RESPONSE_BYTES)
        except httpx.TimeoutException as error:
            raise UpstreamError("COMPUTE_TIMEOUT") from error
        except UpstreamError:
            raise
        except httpx.HTTPError as error:
            raise UpstreamError("COMPUTE_NODE_UNAVAILABLE") from error
        forwarded = {
            key: value
            for key, value in response.headers.items()
            if key.lower() in {"x-fsd-width", "x-fsd-height", "x-fsd-pixel-format"}
        }
        return body, response.headers.get("content-type", "application/octet-stream"), forwarded

    async def create_run(self, node: ComputeNode, envelope: dict[str, object]) -> UpstreamReply:
        return await self._json(node, "POST", "/compute/v1/runs", payload=envelope, expected={202})

    async def run_status(self, node: ComputeNode, node_run_id: str) -> UpstreamReply:
        return await self._json(
            node, "GET", f"/compute/v1/runs/{quote(node_run_id, safe='')}", expected={200}
        )

    async def cancel_run(self, node: ComputeNode, node_run_id: str) -> UpstreamReply:
        return await self._json(
            node,
            "POST",
            f"/compute/v1/runs/{quote(node_run_id, safe='')}/cancel",
            payload={},
            expected={202},
        )

    async def manifest(self, node: ComputeNode, node_run_id: str) -> UpstreamReply:
        return await self._json(
            node, "GET", f"/compute/v1/runs/{quote(node_run_id, safe='')}/manifest", expected={200}
        )

    async def stream_artifact(
        self, node: ComputeNode, node_artifact_id: str, byte_range: str | None
    ) -> tuple[int, str, dict[str, str], AsyncIterator[bytes]]:
        headers = self._headers()
        if byte_range:
            headers["Range"] = byte_range
        client = httpx.AsyncClient(
            timeout=httpx.Timeout(self._settings.artifact_read_timeout_seconds), trust_env=False
        )
        request = client.build_request(
            "GET", self._url(node, "/compute/v1/artifacts"), params={"artifactId": node_artifact_id}, headers=headers
        )
        try:
            response = await client.send(request, stream=True)
        except httpx.TimeoutException as error:
            await client.aclose()
            raise UpstreamError("COMPUTE_TIMEOUT") from error
        except httpx.HTTPError as error:
            await client.aclose()
            raise UpstreamError("COMPUTE_NODE_UNAVAILABLE") from error
        if response.status_code not in {200, 206}:
            try:
                self._raise_response(response)
            finally:
                await response.aclose()
                await client.aclose()
        response_headers = {
            key: value
            for key, value in response.headers.items()
            if key.lower() in {"content-length", "content-range", "accept-ranges", "etag"}
        }

        async def chunks() -> AsyncIterator[bytes]:
            try:
                async for chunk in response.aiter_bytes():
                    if chunk:
                        yield chunk
            finally:
                await response.aclose()
                await client.aclose()

        return response.status_code, response.headers.get("content-type", "application/octet-stream"), response_headers, chunks()

    async def _json(
        self,
        node: ComputeNode,
        method: str,
        route: str,
        *,
        payload: dict[str, object] | None = None,
        expected: set[int],
    ) -> UpstreamReply:
        timeout = httpx.Timeout(
            self._settings.create_read_timeout_seconds,
            connect=self._settings.create_connect_timeout_seconds,
        )
        try:
            async with (
                httpx.AsyncClient(timeout=timeout, trust_env=False) as client,
                client.stream(
                    method, self._url(node, route), headers=self._headers(), json=payload
                ) as response,
            ):
                if response.status_code not in expected:
                    self._raise_response(response)
                body_bytes = await self._read_bounded(response, MAX_JSON_RESPONSE_BYTES)
        except httpx.TimeoutException as error:
            raise UpstreamError("COMPUTE_TIMEOUT") from error
        except UpstreamError:
            raise
        except httpx.HTTPError as error:
            raise UpstreamError("COMPUTE_NODE_UNAVAILABLE") from error
        try:
            body = json.loads(body_bytes)
        except ValueError as error:
            raise UpstreamError("COMPUTE_INVALID_RESPONSE", status_code=response.status_code) from error
        if not isinstance(body, dict):
            raise UpstreamError("COMPUTE_INVALID_RESPONSE", status_code=response.status_code)
        return UpstreamReply(response.status_code, body, dict(response.headers))

    @staticmethod
    async def _read_bounded(response: httpx.Response, cap: int) -> bytes:
        """Read the streamed body, aborting once it exceeds `cap` bytes."""
        chunks: list[bytes] = []
        total = 0
        async for chunk in response.aiter_bytes():
            if not chunk:
                continue
            total += len(chunk)
            if total > cap:
                raise UpstreamError("COMPUTE_INVALID_RESPONSE", status_code=response.status_code)
            chunks.append(chunk)
        return b"".join(chunks)

    @staticmethod
    def _raise_response(response: httpx.Response) -> None:
        if response.status_code in {401, 403}:
            raise UpstreamError("COMPUTE_UPSTREAM_AUTH_FAILED", status_code=response.status_code)
        if response.status_code == 404:
            raise UpstreamError("COMPUTE_RUN_NOT_FOUND", status_code=404)
        if response.status_code == 409:
            raise UpstreamError("COMPUTE_CONFLICT", status_code=409)
        if response.status_code in {400, 422}:
            raise UpstreamError("COMPUTE_REJECTED", status_code=response.status_code)
        raise UpstreamError("COMPUTE_UNAVAILABLE", status_code=response.status_code)
