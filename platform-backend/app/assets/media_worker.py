"""M3 derivative generation after a verified private master exists."""

from __future__ import annotations

import asyncio
import hashlib
import json
import shutil
import struct
import tempfile
import warnings
from dataclasses import dataclass
from pathlib import Path
from uuid import UUID, uuid5

from PIL import Image, ImageDraw, UnidentifiedImageError

from app.assets import repository
from app.assets.cleanup_service import queue_object_cleanup
from app.core import audit_writer
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.infrastructure.storage.object_storage import ObjectStorage
from app.outbox.models import OutboxEvent, RetryableOutboxError


_PUBLIC_PREVIEW_NAMESPACE = UUID("38d4be45-5a1d-49c2-8b58-73271e7148b4")
_MAX_IMAGE_PIXELS = 40_000_000


class MediaProcessingError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class _Derivative:
    purpose: str
    path: Path
    filename: str
    media_type: str


class MediaWorker:
    def __init__(
        self, *, storage: ObjectStorage | None = None, settings: Settings | None = None
    ) -> None:
        self._settings = settings or get_settings()
        self._storage = storage or ObjectStorage(self._settings)

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
            await self._mark_ready(asset_id=asset_id, expected_purposes=expected_purposes)
            return
        if not expected_purposes:
            return

        temp_dir = Path(tempfile.mkdtemp(prefix="fractal-media-", dir=self._settings.media_temp_dir))
        try:
            suffix = {
                "image/png": ".png",
                "video/mp4": ".mp4",
                "model/gltf-binary": ".glb",
                "application/sla": ".stl",
            }.get(source.master_media_type)
            if suffix is None:
                raise MediaProcessingError("unsupported_master_media_type")
            master_path = temp_dir / f"master{suffix}"
            await self._storage.download_file(object_key=source.master_object_key, destination=master_path)
            if master_path.stat().st_size > self._settings.media_max_input_bytes:
                raise MediaProcessingError("media_input_too_large")
            derivatives = await self._generate(source.media_type, master_path, temp_dir)
            missing = [item for item in derivatives if item.purpose not in source.existing_purposes]
            await self._upload_and_record(
                asset_id=asset_id, derivatives=missing, expected_purposes=expected_purposes
            )
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @staticmethod
    def _expected_purposes(media_type: str) -> frozenset[str]:
        if media_type == "image":
            return frozenset({"thumbnail", "watermarked_preview"})
        if media_type == "video":
            return frozenset({"video_poster", "watermarked_preview"})
        if media_type == "mesh":
            return frozenset({"thumbnail", "watermarked_preview"})
        return frozenset()

    async def _upload_and_record(
        self,
        *,
        asset_id: UUID,
        derivatives: list[_Derivative],
        expected_purposes: frozenset[str],
    ) -> None:
        if not derivatives:
            await self._mark_ready(asset_id=asset_id, expected_purposes=expected_purposes)
            return
        stored: list[tuple[_Derivative, UUID, str, str, int]] = []
        try:
            for item in derivatives:
                file_id = uuid5(_PUBLIC_PREVIEW_NAMESPACE, f"{asset_id}:{item.purpose}")
                object_key = f"public/previews/{asset_id}/{file_id}/{item.filename}"
                sha256, size_bytes = await asyncio.to_thread(self._sha256_and_size, item.path)
                if size_bytes > self._settings.media_max_derivative_bytes:
                    raise MediaProcessingError("media_derivative_too_large")
                await self._storage.upload_file(
                    object_key=object_key, source=item.path, media_type=item.media_type
                )
                stored.append((item, file_id, object_key, sha256, size_bytes))
            async with get_engine().begin() as connection:
                for item, file_id, object_key, sha256, size_bytes in stored:
                    if not await repository.add_derivative(
                        connection,
                        asset_id=asset_id,
                        purpose=item.purpose,
                        file_id=file_id,
                        object_key=object_key,
                        sha256=sha256,
                        size_bytes=size_bytes,
                        media_type=item.media_type,
                    ):
                        raise MediaProcessingError("asset_not_ready_for_derivative")
                if not await repository.mark_derivatives_ready_if_complete(
                    connection, asset_id=asset_id, expected_purposes=expected_purposes
                ):
                    raise MediaProcessingError("required_derivatives_missing")
        except Exception:
            if stored:
                async with get_engine().begin() as connection:
                    await queue_object_cleanup(
                        connection, object_keys=[item[2] for item in stored]
                    )
            raise

    async def _mark_ready(self, *, asset_id: UUID, expected_purposes: frozenset[str]) -> None:
        async with get_engine().begin() as connection:
            await repository.mark_derivatives_ready_if_complete(
                connection, asset_id=asset_id, expected_purposes=expected_purposes
            )

    async def _generate(self, media_type: str, master_path: Path, temp_dir: Path) -> list[_Derivative]:
        if media_type == "image":
            return await asyncio.to_thread(self._image_derivatives, master_path, temp_dir)
        if media_type == "video":
            return await self._video_derivatives(master_path, temp_dir)
        if media_type == "mesh":
            return await asyncio.to_thread(self._mesh_derivatives, master_path, temp_dir)
        return []

    @staticmethod
    def _image_derivatives(master_path: Path, temp_dir: Path) -> list[_Derivative]:
        Image.MAX_IMAGE_PIXELS = _MAX_IMAGE_PIXELS
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("error", Image.DecompressionBombWarning)
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

    @staticmethod
    def _mesh_derivatives(master_path: Path, temp_dir: Path) -> list[_Derivative]:
        points = MediaWorker._mesh_points(master_path)
        if len(points) < 3:
            raise MediaProcessingError("invalid_mesh_master")
        preview = MediaWorker._draw_mesh_preview(points)
        thumbnail = preview.copy()
        thumbnail.thumbnail((512, 512), Image.Resampling.LANCZOS)
        thumbnail_path = temp_dir / "thumbnail.png"
        thumbnail.save(thumbnail_path, "PNG", optimize=True)
        preview_path = temp_dir / "watermarked-preview.png"
        preview.save(preview_path, "PNG", optimize=True)
        return [
            _Derivative("thumbnail", thumbnail_path, "thumbnail.png", "image/png"),
            _Derivative("watermarked_preview", preview_path, "watermarked-preview.png", "image/png"),
        ]

    @staticmethod
    def _mesh_points(path: Path) -> list[tuple[float, float, float]]:
        data = path.read_bytes()
        if path.suffix.lower() == ".stl":
            return MediaWorker._stl_points(data)
        if path.suffix.lower() == ".glb":
            return MediaWorker._glb_points(data)
        raise MediaProcessingError("unsupported_mesh_master")

    @staticmethod
    def _stl_points(data: bytes) -> list[tuple[float, float, float]]:
        if len(data) >= 84:
            triangles = struct.unpack_from("<I", data, 80)[0]
            expected_size = 84 + triangles * 50
            if expected_size == len(data) and triangles <= 1_000_000:
                points: list[tuple[float, float, float]] = []
                for offset in range(84, expected_size, 50):
                    for vertex_offset in (12, 24, 36):
                        points.append(struct.unpack_from("<fff", data, offset + vertex_offset))
                return points
        points = []
        for line in data.decode("utf-8", errors="ignore").splitlines():
            fields = line.strip().split()
            if len(fields) == 4 and fields[0].lower() == "vertex":
                try:
                    points.append((float(fields[1]), float(fields[2]), float(fields[3])))
                except ValueError:
                    continue
        return points[:3_000_000]

    @staticmethod
    def _glb_points(data: bytes) -> list[tuple[float, float, float]]:
        if len(data) < 20 or struct.unpack_from("<I", data, 0)[0] != 0x46546C67:
            raise MediaProcessingError("invalid_glb_master")
        if struct.unpack_from("<I", data, 4)[0] != 2 or struct.unpack_from("<I", data, 8)[0] != len(data):
            raise MediaProcessingError("invalid_glb_master")
        offset = 12
        document: dict[str, object] | None = None
        binary: bytes | None = None
        while offset + 8 <= len(data):
            length, chunk_type = struct.unpack_from("<II", data, offset)
            offset += 8
            if length < 0 or offset + length > len(data):
                raise MediaProcessingError("invalid_glb_master")
            chunk = data[offset : offset + length]
            offset += length
            if chunk_type == 0x4E4F534A:
                document = json.loads(chunk.decode("utf-8"))
            elif chunk_type == 0x004E4942:
                binary = chunk
        if document is None or binary is None:
            raise MediaProcessingError("invalid_glb_master")
        try:
            meshes = document["meshes"]
            accessors = document["accessors"]
            views = document["bufferViews"]
            primitive = meshes[0]["primitives"][0]
            accessor = accessors[primitive["attributes"]["POSITION"]]
            view = views[accessor["bufferView"]]
            count = int(accessor["count"])
            if accessor["componentType"] != 5126 or accessor["type"] != "VEC3" or not 0 < count <= 1_000_000:
                raise ValueError
            start = int(view.get("byteOffset", 0)) + int(accessor.get("byteOffset", 0))
            stride = int(view.get("byteStride", 12))
            if stride < 12 or start + (count - 1) * stride + 12 > len(binary):
                raise ValueError
            return [struct.unpack_from("<fff", binary, start + index * stride) for index in range(count)]
        except (KeyError, IndexError, TypeError, ValueError, struct.error) as error:
            raise MediaProcessingError("invalid_glb_master") from error

    @staticmethod
    def _draw_mesh_preview(points: list[tuple[float, float, float]]) -> Image.Image:
        finite = [point for point in points if all(abs(value) < 1e20 for value in point)]
        if len(finite) < 3:
            raise MediaProcessingError("invalid_mesh_master")
        sample = finite[:: max(1, len(finite) // 20_000)]
        projected = [(0.866 * x - 0.5 * y, 0.5 * x + 0.866 * y - z) for x, y, z in sample]
        min_x, max_x = min(x for x, _ in projected), max(x for x, _ in projected)
        min_y, max_y = min(y for _, y in projected), max(y for _, y in projected)
        scale = max(max_x - min_x, max_y - min_y, 1e-9)
        image = Image.new("RGBA", (1600, 1600), (9, 14, 27, 255))
        draw = ImageDraw.Draw(image)
        padding = 100
        for x, y in projected:
            px = padding + (x - min_x) / scale * (1600 - padding * 2)
            py = padding + (max_y - y) / scale * (1600 - padding * 2)
            draw.ellipse((px - 1, py - 1, px + 1, py + 1), fill=(86, 207, 255, 170))
        MediaWorker._watermark(image)
        return image

    @staticmethod
    def _watermark(image: Image.Image) -> None:
        overlay = Image.new("RGBA", image.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(overlay)
        text = "Fractal Studio"
        try:
            from PIL import ImageFont

            font = ImageFont.truetype("DejaVuSans.ttf", max(16, min(image.size) // 18))
        except OSError:
            font = None
        bounds = draw.textbbox((0, 0), text, font=font)
        padding = 12
        x = max(padding, image.width - (bounds[2] - bounds[0]) - padding * 2)
        y = max(padding, image.height - (bounds[3] - bounds[1]) - padding * 2)
        draw.rounded_rectangle(
            (x - padding, y - padding, image.width - padding, image.height - padding),
            radius=8,
            fill=(0, 0, 0, 120),
        )
        draw.text((x, y), text, fill=(255, 255, 255, 210), font=font)
        image.alpha_composite(overlay)

    async def _video_derivatives(self, master_path: Path, temp_dir: Path) -> list[_Derivative]:
        poster_path = temp_dir / "video-poster.jpg"
        preview_path = temp_dir / "watermarked-preview.mp4"
        watermark_path = temp_dir / "watermark.png"
        await self._run_ffmpeg(
            "-ss", "0", "-i", str(master_path), "-frames:v", "1", "-vf",
            "scale='min(1280,iw)':-2", "-q:v", "3", str(poster_path),
        )
        await asyncio.to_thread(self._video_watermark, watermark_path)
        filter_graph = (
            "[0:v]scale=trunc(min(1280\\,iw)/2)*2:-2[base];"
            "[1:v]format=rgba[watermark];"
            "[base][watermark]overlay=W-w-24:H-h-24"
        )
        await self._run_ffmpeg(
            "-i", str(master_path), "-loop", "1", "-i", str(watermark_path), "-t", "30",
            "-filter_complex", filter_graph, "-shortest", "-c:v", "libx264", "-pix_fmt", "yuv420p",
            "-movflags", "+faststart", str(preview_path),
        )
        return [
            _Derivative("video_poster", poster_path, "video-poster.jpg", "image/jpeg"),
            _Derivative(
                "watermarked_preview", preview_path, "watermarked-preview.mp4", "video/mp4"
            ),
        ]

    @staticmethod
    def _video_watermark(path: Path) -> None:
        image = Image.new("RGBA", (520, 88), (0, 0, 0, 120))
        draw = ImageDraw.Draw(image)
        try:
            from PIL import ImageFont

            font = ImageFont.truetype("DejaVuSans.ttf", 38)
        except OSError:
            font = None
        draw.text((20, 20), "Fractal Studio", fill=(255, 255, 255, 220), font=font)
        image.save(path, "PNG", optimize=True)

    async def _run_ffmpeg(self, *arguments: str) -> None:
        try:
            process = await asyncio.create_subprocess_exec(
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *arguments,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE,
            )
        except FileNotFoundError as error:
            raise MediaProcessingError("ffmpeg_unavailable") from error
        try:
            _stdout, stderr = await asyncio.wait_for(
                process.communicate(), timeout=self._settings.media_ffmpeg_timeout_seconds
            )
        except TimeoutError as error:
            process.kill()
            await process.communicate()
            raise MediaProcessingError("ffmpeg_timeout") from error
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

    async def dead_letter(self, event: OutboxEvent, error_code: str) -> None:
        try:
            asset_id = UUID(str(event.payload["assetId"]))
        except (KeyError, TypeError, ValueError):
            return
        async with get_engine().begin() as connection:
            changed = await repository.mark_derivatives_failed(
                connection, asset_id=asset_id, error_code=f"outbox_{error_code}"
            )
            if changed:
                await audit_writer.record_system_action(
                    connection,
                    action="asset.derivatives_dead_lettered",
                    subject_type="asset",
                    subject_id=asset_id,
                    request_id_value=event.causation_request_id or f"outbox:{event.id}",
                    metadata={"eventType": event.event_type, "errorCode": error_code},
                )
