"""T08 safe DTO and image-derivative behaviour without external services."""

from __future__ import annotations

from datetime import UTC, datetime
import json
from pathlib import Path
import shutil
import struct
import subprocess
from uuid import uuid4

import pytest
from PIL import Image

from app.assets.media_worker import MediaWorker
from app.assets import repository
from app.assets.reader import AssetReadService
from app.assets.repository import AssetReadRecord, AssetRecord
from app.assets.service import asset_view
from app.core.config import Settings


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


def test_mesh_derivatives_create_required_png_previews(tmp_path: Path) -> None:
    master = tmp_path / "master.stl"
    master.write_text(
        """solid triangle
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 1 0 0
vertex 0 1 0
endloop
endfacet
endsolid triangle
"""
    )
    derivatives = MediaWorker._mesh_derivatives(master, tmp_path)
    assert {item.purpose for item in derivatives} == {"thumbnail", "watermarked_preview"}
    for item in derivatives:
        with Image.open(item.path) as image:
            assert image.format == "PNG"


def test_glb_mesh_derivatives_create_required_png_previews(tmp_path: Path) -> None:
    positions = struct.pack("<fffffffff", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
    document = json.dumps(
        {
            "asset": {"version": "2.0"},
            "buffers": [{"byteLength": len(positions)}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions)}],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}
            ],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        },
        separators=(",", ":"),
    ).encode()
    document += b" " * ((4 - len(document) % 4) % 4)
    binary = positions + b"\x00" * ((4 - len(positions) % 4) % 4)
    master = tmp_path / "master.glb"
    master.write_bytes(
        struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(document) + 8 + len(binary))
        + struct.pack("<II", len(document), 0x4E4F534A)
        + document
        + struct.pack("<II", len(binary), 0x004E4942)
        + binary
    )
    assert {item.purpose for item in MediaWorker._mesh_derivatives(master, tmp_path)} == {
        "thumbnail",
        "watermarked_preview",
    }


@pytest.mark.asyncio
async def test_video_derivatives_create_poster_and_watermarked_preview(tmp_path: Path) -> None:
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is not installed")
    master = tmp_path / "master.mp4"
    subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=blue:s=64x64:d=1",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-y",
            str(master),
        ],
        check=True,
    )
    settings = Settings(database_url="postgresql+asyncpg://unused", session_secret="x" * 32)
    derivatives = await MediaWorker(settings=settings)._video_derivatives(master, tmp_path)
    assert {item.purpose for item in derivatives} == {"video_poster", "watermarked_preview"}
    assert all(item.path.stat().st_size > 0 for item in derivatives)


@pytest.mark.asyncio
async def test_asset_reader_signs_safe_preview_urls_without_returning_object_keys() -> None:
    class Storage:
        async def create_signed_get_url(self, *, object_key: str, expires_seconds: int) -> str:
            assert expires_seconds == 3600
            return f"https://cdn.example.test/{object_key}?signature=opaque"

    settings = Settings(database_url="postgresql+asyncpg://unused", session_secret="x" * 32)
    reader = AssetReadService(object_storage=Storage(), settings=settings)  # type: ignore[arg-type]
    record = AssetReadRecord(
        id=uuid4(),
        owner_id=uuid4(),
        media_type="image",
        status="ready",
        visibility="private",
        derivative_keys={
            "thumbnail": "public/previews/asset/thumbnail.png",
            "watermarked_preview": "public/previews/asset/watermark.png",
        },
    )
    preview = await reader._preview(record)
    assert preview.thumbnail_url is not None and "signature=opaque" in preview.thumbnail_url
    assert preview.watermarked_preview_url is not None
    assert "object_key" not in repr(preview).lower()


@pytest.mark.asyncio
async def test_add_derivative_refuses_asset_that_is_no_longer_ready() -> None:
    class Result:
        def scalar_one_or_none(self):
            return None

    class Connection:
        def __init__(self) -> None:
            self.calls = 0

        async def execute(self, _statement, _params):
            self.calls += 1
            return Result()

    connection = Connection()
    inserted = await repository.add_derivative(
        connection,  # type: ignore[arg-type]
        asset_id=uuid4(),
        purpose="thumbnail",
        file_id=uuid4(),
        object_key="public/previews/x/thumbnail.png",
        sha256="0" * 64,
        size_bytes=1,
        media_type="image/png",
    )
    assert not inserted
    assert connection.calls == 1


@pytest.mark.asyncio
async def test_default_library_query_excludes_hidden_assets() -> None:
    class Rows:
        def all(self):
            return []

    class Result:
        def mappings(self):
            return Rows()

    class Connection:
        statement = ""

        async def execute(self, statement, _params):
            self.statement = str(statement)
            return Result()

    connection = Connection()
    await repository.list_owned(connection, owner_id=uuid4(), limit=1)  # type: ignore[arg-type]
    assert "a.visibility = 'private'" in connection.statement
