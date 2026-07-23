from __future__ import annotations

import re
from pathlib import Path

import pytest


@pytest.fixture
def studio_root(pytestconfig: pytest.Config) -> Path:
    return Path(pytestconfig.getoption("--studio-root")).resolve()


def registered_kinds(source: str) -> set[str]:
    registry = source.split("COMPUTE_CAPABILITIES = {{", 1)[1].split("}};", 1)[0]
    return set(re.findall(r'^\s*\{"([a-z0-9_]+)"', registry, re.MULTILINE))


def documented_kinds(reference: str) -> set[str]:
    return set(re.findall(r"^## kind: ([a-z0-9_]+)$", reference, re.MULTILINE))


def test_every_registered_kind_has_exactly_one_job_reference(studio_root: Path):
    source = (studio_root / "backend/src/api/routes_compute_v1.cpp").read_text()
    reference = (studio_root / "docs/compute_v1_jobs.md").read_text()
    headings = re.findall(r"^## kind: ([a-z0-9_]+)$", reference, re.MULTILINE)
    assert len(headings) == len(set(headings))
    assert documented_kinds(reference) == registered_kinds(source)


def test_contract_documents_every_private_endpoint(studio_root: Path):
    contract = (studio_root / "docs/compute_v1_contract.md").read_text()
    paths = {
        "/compute/v1/health", "/compute/v1/capabilities",
        "/compute/v1/previews", "/compute/v1/runs",
        "/compute/v1/runs/{id}", "/compute/v1/runs/{id}/cancel",
        "/compute/v1/runs/{id}/manifest", "/compute/v1/artifacts?artifactId=...",
    }
    assert all(path in contract for path in paths)


def test_platform_guide_covers_worker_safety_invariants(studio_root: Path):
    guide = (studio_root / "docs/platform_compute_integration.md").read_text()
    required = {
        "FOR UPDATE SKIP LOCKED", "idempotency", "compute_node_id",
        "compute_run_id", "kernelReported", "SHA-256", "cancel",
    }
    assert all(term in guide for term in required)
