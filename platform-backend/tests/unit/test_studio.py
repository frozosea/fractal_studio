"""M2 deterministic canonicalisation, mapping and no-DB preview checks."""

from __future__ import annotations

import io
from uuid import UUID

import pytest
from PIL import Image

from app.core.config import Settings
from app.infrastructure.compute.compute_client import InlineComputeFrame
from app.infrastructure.redis.quota_store import PreviewQuotaUnavailable
from app.studio.compute_request_mapper import PREVIEW_MAPPING_VERSION, map_preview_v1
from app.studio.models import FractalSpec
from app.studio.preview_service import PreviewService
from app.studio.recipe_service import canonicalize_spec


class AllowedLimiter:
    async def allow(self, _user_id: UUID) -> bool:
        return True


class InlineFrameClient:
    def __init__(self, frame: InlineComputeFrame) -> None:
        self.frame = frame
        self.requests: list[dict[str, object]] = []

    async def render_map_inline(self, request_body: dict[str, object], *, timeout_seconds: float) -> InlineComputeFrame:
        assert timeout_seconds == 5.0
        self.requests.append(request_body)
        return self.frame


class UnavailableLimiter:
    async def allow(self, _user_id: UUID) -> bool:
        raise PreviewQuotaUnavailable("redis_unavailable")


def _settings() -> Settings:
    return Settings(
        database_url="postgresql+asyncpg://unused",
        session_secret="test-session-secret-long-enough",
        compute_service_key="test-key",
    )


def test_canonical_recipe_hash_ignores_json_key_order_and_fills_defaults() -> None:
    first = canonicalize_spec(FractalSpec.model_validate({"version": 1, "seed": 42, "scale": 4}))
    second = canonicalize_spec(FractalSpec.model_validate({"seed": 42, "version": 1, "scale": 4.0}))

    assert first.sha256 == second.sha256
    assert first.spec == second.spec
    assert first.spec["centerRe"] == 0.0
    assert first.spec["iterations"] == 256


def test_preview_mapper_is_pure_and_versioned() -> None:
    canonical = canonicalize_spec(FractalSpec.model_validate({"version": 1, "seed": 42}))
    request_id = UUID("12345678-1234-5678-1234-567812345678")

    request = map_preview_v1(canonical.spec, width=64, height=64, request_id=request_id)

    assert PREVIEW_MAPPING_VERSION == "compute-v1-preview-v1"
    assert request == {
        "schemaVersion": 1,
        "kind": "map_image",
        "payload": {
            "width": 64,
            "height": 64,
            "iterations": 256,
            "variant": "mandelbrot",
            "centerRe": 0.0,
            "centerIm": 0.0,
            "scale": 4.0,
            "julia": False,
            "bailout": 4.0,
            "metric": "escape",
            "smooth": False,
            "rotationDeg": 0.0,
            "pairwiseCap": 64,
            "engine": "auto",
            "scalarType": "auto",
        },
    }
    assert canonical.spec["seed"] == 42


def test_mapper_preserves_optional_color_map() -> None:
    canonical = canonicalize_spec(FractalSpec.model_validate({"version": 1, "colorMap": "viridis"}))
    request = map_preview_v1(canonical.spec, width=64, height=64, request_id=UUID(int=1))

    assert request["payload"]["colorMap"] == "viridis"


def test_mapper_preserves_advanced_2d_controls_and_gradient() -> None:
    canonical = canonicalize_spec(FractalSpec.model_validate({
        "version": 1, "metric": "min_pairwise_dist", "smooth": True,
        "rotationDeg": 15, "pairwiseCap": 128, "colorMap": None,
        "colorProgram": {"stops": [{"at": 0, "color": "#000000"}, {"at": 1, "color": "#ffffff"}]},
        "engine": "avx2", "scalarType": "fp64",
    }))
    request = map_preview_v1(canonical.spec, width=64, height=64, request_id=UUID(int=1))

    assert request["payload"]["metric"] == "min_pairwise_dist"
    assert request["payload"]["smooth"] is True
    assert request["payload"]["rotationDeg"] == 15.0
    assert request["payload"]["pairwiseCap"] == 128
    assert request["payload"]["colorProgram"]["stops"][1]["color"] == "#ffffff"
    assert request["payload"]["engine"] == "avx2"


@pytest.mark.asyncio
async def test_preview_encodes_compute_rgba_to_png_without_durable_dependency() -> None:
    client = InlineFrameClient(InlineComputeFrame(rgba=bytes([255, 0, 0, 255]) * 4096, width=64, height=64))
    service = PreviewService(settings=_settings(), rate_limiter=AllowedLimiter(), compute_client=client)  # type: ignore[arg-type]
    canonical = canonicalize_spec(FractalSpec.model_validate({"version": 1, "seed": 42}))

    png = await service.render(owner_id=UUID(int=1), canonical=canonical, width=64, height=64)

    assert png.startswith(b"\x89PNG\r\n\x1a\n")
    assert Image.open(io.BytesIO(png)).size == (64, 64)
    assert client.requests[0]["payload"]["width"] == 64


@pytest.mark.asyncio
async def test_preview_rejects_oversized_request_before_redis_or_compute() -> None:
    client = InlineFrameClient(InlineComputeFrame(rgba=b"", width=0, height=0))
    service = PreviewService(settings=_settings(), rate_limiter=AllowedLimiter(), compute_client=client)  # type: ignore[arg-type]
    canonical = canonicalize_spec(FractalSpec.model_validate({"version": 1}))

    with pytest.raises(Exception) as error:
        await service.render(owner_id=UUID(int=1), canonical=canonical, width=1025, height=1)

    assert getattr(error.value, "status_code", None) == 422
    assert client.requests == []


@pytest.mark.asyncio
async def test_preview_fails_closed_when_redis_is_unavailable() -> None:
    client = InlineFrameClient(InlineComputeFrame(rgba=bytes([0, 0, 0, 255]) * 4096, width=64, height=64))
    service = PreviewService(settings=_settings(), rate_limiter=UnavailableLimiter(), compute_client=client)  # type: ignore[arg-type]
    canonical = canonicalize_spec(FractalSpec.model_validate({"version": 1}))

    with pytest.raises(Exception) as error:
        await service.render(owner_id=UUID(int=1), canonical=canonical, width=64, height=64)

    assert getattr(error.value, "status_code", None) == 503
    assert client.requests == []
