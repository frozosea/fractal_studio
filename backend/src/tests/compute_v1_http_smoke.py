#!/usr/bin/env python3
"""End-to-end contract smoke test for the private Compute v1 HTTP surface."""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


SERVICE_KEY = "compute-v1-http-smoke-key"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(url: str, *, body: dict | None = None, authorized: bool = True) -> tuple[int, bytes, dict]:
    headers = {}
    data = None
    if authorized:
        headers["Authorization"] = f"Bearer {SERVICE_KEY}"
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body, separators=(",", ":")).encode()
    req = urllib.request.Request(url, data=data, headers=headers, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, response.read(), dict(response.headers)
    except urllib.error.HTTPError as error:
        return error.code, error.read(), dict(error.headers)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: compute_v1_http_smoke.py BACKEND_BINARY STUDIO_ROOT")
    binary = Path(sys.argv[1]).resolve()
    studio_root = Path(sys.argv[2]).resolve()
    port = free_port()
    env = os.environ.copy()
    env.update({
        "FSD_STARTUP_BENCHMARK": "off",
        "FSD_COMPUTE_SERVICE_KEY": SERVICE_KEY,
        "FSD_RENDERER_VERSION": "contract-smoke",
    })
    process = subprocess.Popen(
        [str(binary), str(port)], cwd=studio_root,
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, start_new_session=True,
    )
    base = f"http://127.0.0.1:{port}"
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise AssertionError(f"backend exited early:\n{process.stdout.read() if process.stdout else ''}")
            try:
                status, payload, _ = request(f"{base}/compute/v1/health", authorized=False)
                if status == 200 and json.loads(payload)["status"] == "ok":
                    break
            except OSError:
                pass
            time.sleep(0.05)
        else:
            raise AssertionError("backend did not become ready")

        status, payload, _ = request(f"{base}/compute/v1/capabilities", authorized=False)
        assert status == 401
        assert json.loads(payload)["error"]["code"] == "COMPUTE_UNAUTHORIZED"

        status, payload, _ = request(f"{base}/compute/v1/capabilities")
        assert status == 200
        capabilities = json.loads(payload)
        assert "map_image" in capabilities["persistentKinds"]

        map_payload = {
            "centerRe": -0.75, "centerIm": 0, "scale": 3,
            "width": 64, "height": 64, "iterations": 32,
            "variant": "mandelbrot", "engine": "openmp", "scalarType": "fp64",
        }
        status, frame, headers = request(
            f"{base}/compute/v1/previews",
            body={"schemaVersion": 1, "kind": "map_image", "payload": map_payload},
        )
        assert status == 200
        assert headers.get("Content-Type") == "application/octet-stream"
        assert len(frame) == 64 * 64 * 4

        run_request = {
            "schemaVersion": 1, "kind": "map_image",
            "idempotencyKey": f"compute-v1-smoke:{port}", "payload": map_payload,
        }
        status, payload, _ = request(
            f"{base}/compute/v1/runs",
            body=run_request,
        )
        assert status == 202
        run_id = json.loads(payload)["data"]["computeRunId"]
        status, duplicate_payload, _ = request(f"{base}/compute/v1/runs", body=run_request)
        assert status == 202
        assert json.loads(duplicate_payload)["data"]["computeRunId"] == run_id

        run_status = ""
        for _ in range(100):
            status, payload, _ = request(f"{base}/compute/v1/runs/{run_id}")
            assert status == 200
            run_status = json.loads(payload)["data"]["status"]
            if run_status in {"completed", "failed", "cancelled"}:
                break
            time.sleep(0.05)
        assert run_status == "completed"

        status, payload, _ = request(f"{base}/compute/v1/runs/{run_id}/manifest")
        assert status == 200
        manifest = json.loads(payload)
        assert manifest["rendererVersion"] == "contract-smoke"
        assert manifest["escapeAnalysis"]["certifiedRadius"] is None
        assert manifest["artifacts"]
        artifact = next(item for item in manifest["artifacts"] if item["mediaType"] == "image/png")
        assert len(artifact["sha256"]) == 64

        query = urllib.parse.urlencode({"artifactId": artifact["artifactId"]})
        status, content, headers = request(f"{base}/compute/v1/artifacts?{query}")
        assert status == 200
        assert headers.get("Content-Type") == "image/png"
        assert content.startswith(b"\x89PNG\r\n\x1a\n")
        return 0
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
