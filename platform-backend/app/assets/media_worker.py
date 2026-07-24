"""M3 derivative generation after a verified private master exists."""

from __future__ import annotations

import asyncio
import hashlib
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from uuid import UUID, uuid5

from PIL import Image, ImageDraw, UnidentifiedImageError

from app.assets import repository
from app.core.db import get_engine
from app.infrastructure.storage.object_storage import ObjectStorage
from app.outbox.models import OutboxEvent, RetryableOutboxError


_PUBLIC_PREVIEW_NAMESPACE = UUID("38d4be45-5a1d-49c2-8b58-73271e7148b4")
_MAX_IMAGE_PIXELS = 40_000_000
_FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


class MediaProcessingError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class _Derivative:
    purpose: str
    path: Path
    filename: str
    media_type: str


class MediaWorker:
    def __init__(self, *, storage: ObjectStorage | None = None) -> None:
        self._storage = storage or ObjectStorage()

    async def create_derivatives(self, event: OutboxEvent) -> None:
        try:
            asset_id = UUID(str(event.payload["assetId"]))
        except (KeyError, TypeError, ValueError) as error:
            raise RetryableOutboxError("invalid_media_event") from error
        async with get_engine().connect() as connection:
            source = await repository.find_media_source(connection, asset_id=asset_id)
        if source is None or source.status != "ready":
            return
        expected_purposes = self._expected_purposes(source.media_type)
        if expected_purposes.issubset(source.existing_purposes):
            return
        if not expected_purposes:
            return

        temp_dir = Path(tempfile.mkdtemp(prefix="fractal-media-"))
        try:
            suffix = ".png" if source.master_media_type == "image/png" else ".mp4"
            master_path = temp_dir / f"master{suffix}"
            await self._storage.download_file(object_key=source.master_object_key, destination=master_path)
            derivatives = await self._generate(source.media_type, master_path, temp_dir)
            missing = [item for item in derivatives if item.purpose not in source.existing_purposes]
            await self._upload_and_record(asset_id=asset_id, derivatives=missing)
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @staticmethod
    def _expected_purposes(media_type: str) -> frozenset[str]:
        if media_type == "image":
            return frozenset({"thumbnail", "watermarked_preview"})
        if media_type == "video":
            return frozenset({"video_poster", "watermarked_preview"})
        return frozenset()

    async def _upload_and_record(self, *, asset_id: UUID, derivatives: list[_Derivative]) -> None:
        if not derivatives:
            return
        stored: list[tuple[_Derivative, UUID, str, str, int]] = []
        for item in derivatives:
            file_id = uuid5(_PUBLIC_PREVIEW_NAMESPACE, f"{asset_id}:{item.purpose}")
            object_key = f"public/previews/{asset_id}/{file_id}/{item.filename}"
            sha256, size_bytes = await asyncio.to_thread(self._sha256_and_size, item.path)
            await self._storage.upload_file(
                object_key=object_key, source=item.path, media_type=item.media_type
            )
            stored.append((item, file_id, object_key, sha256, size_bytes))
        async with get_engine().begin() as connection:
            for item, file_id, object_key, sha256, size_bytes in stored:
                await repository.add_derivative(
                    connection,
                    asset_id=asset_id,
                    purpose=item.purpose,
                    file_id=file_id,
                    object_key=object_key,
                    sha256=sha256,
                    size_bytes=size_bytes,
                    media_type=item.media_type,
                )

    async def _generate(self, media_type: str, master_path: Path, temp_dir: Path) -> list[_Derivative]:
        if media_type == "image":
            return await asyncio.to_thread(self._image_derivatives, master_path, temp_dir)
        if media_type == "video":
            return await self._video_derivatives(master_path, temp_dir)
        return []

    @staticmethod
    def _image_derivatives(master_path: Path, temp_dir: Path) -> list[_Derivative]:
        Image.MAX_IMAGE_PIXELS = _MAX_IMAGE_PIXELS
        try:
            with Image.open(master_path) as check:
                check.verify()
            with Image.open(master_path) as image:
                image.load()
                base = image.convert("RGBA")
        except (OSError, UnidentifiedImageError) as error:
            raise MediaProcessingError("invalid_image_master") from error

        thumbnail = base.copy()
        thumbnail.thumbnail((512, 512), Image.Resampling.LANCZOS)
        thumbnail_path = temp_dir / "thumbnail.png"
        thumbnail.save(thumbnail_path, "PNG", optimize=True)

        preview = base.copy()
        preview.thumbnail((1600, 1600), Image.Resampling.LANCZOS)
        overlay = Image.new("RGBA", preview.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(overlay)
        text = "Fractal Studio"
        try:
            from PIL import ImageFont

            font = ImageFont.truetype("DejaVuSans.ttf", max(16, min(preview.size) // 18))
        except OSError:
            font = None
        bounds = draw.textbbox((0, 0), text, font=font)
        padding = 12
        x = max(padding, preview.width - (bounds[2] - bounds[0]) - padding * 2)
        y = max(padding, preview.height - (bounds[3] - bounds[1]) - padding * 2)
        draw.rounded_rectangle(
            (x - padding, y - padding, preview.width - padding, preview.height - padding),
            radius=8,
            fill=(0, 0, 0, 120),
        )
        draw.text((x, y), text, fill=(255, 255, 255, 210), font=font)
        preview = Image.alpha_composite(preview, overlay)
        preview_path = temp_dir / "watermarked-preview.png"
        preview.save(preview_path, "PNG", optimize=True)
        return [
            _Derivative("thumbnail", thumbnail_path, "thumbnail.png", "image/png"),
            _Derivative(
                "watermarked_preview", preview_path, "watermarked-preview.png", "image/png"
            ),
        ]

    async def _video_derivatives(self, master_path: Path, temp_dir: Path) -> list[_Derivative]:
        poster_path = temp_dir / "video-poster.jpg"
        preview_path = temp_dir / "watermarked-preview.mp4"
        await self._run_ffmpeg(
            "-ss", "0", "-i", str(master_path), "-frames:v", "1", "-vf",
            "scale='min(1280,iw)':-2", "-q:v", "3", str(poster_path),
        )
        filter_graph = (
            "scale=trunc(min(1280\\,iw)/2)*2:-2,"
            f"drawtext=fontfile={_FONT_PATH}:text='Fractal Studio':"
            "x=w-tw-24:y=h-th-24:fontcolor=white@0.85:box=1:boxcolor=black@0.45:boxborderw=12"
        )
        await self._run_ffmpeg(
            "-i", str(master_path), "-t", "30", "-vf", filter_graph,
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(preview_path),
        )
        return [
            _Derivative("video_poster", poster_path, "video-poster.jpg", "image/jpeg"),
            _Derivative(
                "watermarked_preview", preview_path, "watermarked-preview.mp4", "video/mp4"
            ),
        ]

    @staticmethod
    async def _run_ffmpeg(*arguments: str) -> None:
        try:
            process = await asyncio.create_subprocess_exec(
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *arguments,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE,
            )
        except FileNotFoundError as error:
            raise MediaProcessingError("ffmpeg_unavailable") from error
        _stdout, stderr = await process.communicate()
        if process.returncode != 0:
            raise MediaProcessingError("ffmpeg_derivative_failed") from RuntimeError(
                stderr.decode(errors="replace")[:500]
            )

    @staticmethod
    def _sha256_and_size(path: Path) -> tuple[str, int]:
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                size += len(chunk)
                digest.update(chunk)
        if size == 0:
            raise MediaProcessingError("empty_derivative")
        return digest.hexdigest(), size
