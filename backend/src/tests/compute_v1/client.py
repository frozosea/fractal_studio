from __future__ import annotations

import json
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class HttpResult:
    status: int
    content: bytes
    headers: dict[str, str]

    def json(self) -> dict[str, Any]:
        return json.loads(self.content)


class ComputeClient:
    def __init__(self, base_url: str, service_key: str) -> None:
        self.base_url = base_url.rstrip("/")
        self.service_key = service_key

    def request(
        self,
        path: str,
        *,
        body: dict[str, Any] | None = None,
        authorized: bool = True,
        headers: dict[str, str] | None = None,
        method: str | None = None,
    ) -> HttpResult:
        request_headers = dict(headers or {})
        if authorized:
            request_headers["Authorization"] = f"Bearer {self.service_key}"
        data = None
        if body is not None:
            request_headers["Content-Type"] = "application/json"
            data = json.dumps(body, separators=(",", ":")).encode()
        request = urllib.request.Request(
            f"{self.base_url}{path}", data=data, headers=request_headers,
            method=method or ("POST" if data is not None else "GET"),
        )
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                return HttpResult(response.status, response.read(), dict(response.headers))
        except urllib.error.HTTPError as error:
            return HttpResult(error.code, error.read(), dict(error.headers))

    def envelope(self, kind: str, payload: dict[str, Any]) -> dict[str, Any]:
        return {"schemaVersion": 1, "kind": kind, "payload": payload}

    def preview(self, kind: str, payload: dict[str, Any]) -> HttpResult:
        return self.request("/compute/v1/previews", body=self.envelope(kind, payload))

    def create_run(
        self, kind: str, payload: dict[str, Any], *, idempotency_key: str | None = None,
    ) -> tuple[str, HttpResult]:
        body = self.envelope(kind, payload)
        body["idempotencyKey"] = idempotency_key or f"pytest:{kind}:{uuid.uuid4()}"
        result = self.request("/compute/v1/runs", body=body)
        assert result.status == 202, result.content
        return result.json()["data"]["computeRunId"], result

    def run_status(self, run_id: str) -> dict[str, Any]:
        result = self.request(f"/compute/v1/runs/{run_id}")
        assert result.status == 200, result.content
        return result.json()["data"]

    def wait_for_run(
        self, run_id: str, *, timeout: float = 10.0,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.run_status(run_id)
            if status["status"] in {"completed", "failed", "cancelled"}:
                return status
            time.sleep(0.02)
        raise AssertionError(f"run {run_id} did not reach a terminal state")

    def completed_manifest(
        self, kind: str, payload: dict[str, Any], *, timeout: float = 10.0,
    ) -> dict[str, Any]:
        _, manifest = self.completed_run(kind, payload, timeout=timeout)
        return manifest

    def completed_run(
        self, kind: str, payload: dict[str, Any], *, timeout: float = 10.0,
    ) -> tuple[str, dict[str, Any]]:
        run_id, _ = self.create_run(kind, payload)
        terminal = self.wait_for_run(run_id, timeout=timeout)
        assert terminal["status"] == "completed", terminal
        return run_id, self.manifest(run_id)

    def manifest(self, run_id: str) -> dict[str, Any]:
        result = self.request(f"/compute/v1/runs/{run_id}/manifest")
        assert result.status == 200, result.content
        return result.json()

    def cancel(self, run_id: str) -> dict[str, Any]:
        result = self.request(f"/compute/v1/runs/{run_id}/cancel", body={})
        assert result.status == 202, result.content
        return result.json()["data"]

    def artifact(self, artifact_id: str, *, byte_range: str | None = None) -> HttpResult:
        query = urllib.parse.urlencode({"artifactId": artifact_id})
        headers = {"Range": byte_range} if byte_range else None
        return self.request(f"/compute/v1/artifacts?{query}", headers=headers)


def artifact_with_media_type(
    manifest: dict[str, Any], media_type: str,
) -> dict[str, Any]:
    return next(
        artifact for artifact in manifest["artifacts"]
        if artifact["mediaType"] == media_type
    )
