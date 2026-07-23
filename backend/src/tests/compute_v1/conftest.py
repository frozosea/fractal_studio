from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

from .client import ComputeClient


SERVICE_KEY = "compute-v1-pytest-key"


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--backend-binary", required=True)
    parser.addoption("--studio-root", required=True)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


@pytest.fixture
def backend(pytestconfig: pytest.Config) -> ComputeClient:
    binary = Path(pytestconfig.getoption("--backend-binary")).resolve()
    studio_root = Path(pytestconfig.getoption("--studio-root")).resolve()
    port = free_port()
    env = os.environ.copy()
    env.update({
        "FSD_STARTUP_BENCHMARK": "off",
        "FSD_COMPUTE_SERVICE_KEY": SERVICE_KEY,
        "FSD_RENDERER_VERSION": "contract-pytest",
    })
    process = subprocess.Popen(
        [str(binary), str(port)], cwd=studio_root, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, start_new_session=True,
    )
    api = ComputeClient(f"http://127.0.0.1:{port}", SERVICE_KEY)
    try:
        for _ in range(100):
            if process.poll() is not None:
                output = process.stdout.read() if process.stdout else ""
                raise AssertionError(f"backend exited early:\n{output}")
            try:
                result = api.request("/compute/v1/health", authorized=False)
                if result.status == 200 and result.json().get("status") == "ok":
                    break
            except OSError:
                pass
            time.sleep(0.05)
        else:
            raise AssertionError("backend did not become ready")
        yield api
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)


@pytest.fixture
def client(backend: ComputeClient) -> ComputeClient:
    return backend
