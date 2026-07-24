"""T08 safe DTO and image-derivative behaviour without external services."""

from __future__ import annotations

from datetime import UTC, datetime
from pathlib import Path
from uuid import uuid4

import pytest
from PIL import Image

from app.assets.media_worker import MediaWorker
from app.assets.ports import DenyEntitlementReader
from app.assets.repository import AssetRecord
from app.assets.service import asset_view


def test_safe_asset_view_hides_manifest_and_storage_internals() -> None:
    record = AssetRecord(
        id=uuid4(),
        owner_id=uuid4(),
        recipe_id=uuid4(),
        media_type="image",
        status="ready",
        visibility="private",
        created_at=datetime.now(UTC),
        files=[
            {"purpose": "master", "mediaType": "image/png", "sizeBytes": 123},
            {"purpose": "render_manifest", "mediaType": "application/json", "sizeBytes": 99},
        ],
    )
    payload = asset_view(record).model_dump(mode="json", by_alias=True)
    assert payload["files"] == [{"purpose": "master", "mediaType": "image/png", "sizeBytes": 123}]
    assert "objectKey" not in str(payload)
    assert "runId" not in str(payload)


def test_image_derivatives_are_png_and_watermarked(tmp_path: Path) -> None:
    master = tmp_path / "master.png"
    Image.new("RGB", (1800, 1000), (12, 34, 56)).save(master, "PNG")
    derivatives = MediaWorker._image_derivatives(master, tmp_path)
    assert {item.purpose for item in derivatives} == {"thumbnail", "watermarked_preview"}
    for item in derivatives:
        with Image.open(item.path) as image:
            assert image.format == "PNG"
    preview = next(item for item in derivatives if item.purpose == "watermarked_preview")
    with Image.open(preview.path) as image:
        assert image.size[0] <= 1600
        assert image.getpixel((image.width - 40, image.height - 40)) != (12, 34, 56, 255)


@pytest.mark.asyncio
async def test_pre_t11_entitlement_reader_denies_non_owner() -> None:
    assert not await DenyEntitlementReader().has_active_entitlement(user_id=uuid4(), asset_id=uuid4())
