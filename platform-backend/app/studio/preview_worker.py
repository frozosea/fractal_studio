"""Concurrent worker for Redis-backed interactive previews."""

from __future__ import annotations

import asyncio
import logging
import signal

from app.core.config import get_settings
from app.infrastructure.compute.compute_client import ComputeClient, ComputeClientError
from app.studio.compute_request_mapper import bound_preview_payload
from app.studio.preview_queue import PreviewQueueUnavailable, RedisPreviewQueue
from app.studio.rgba_png_encoder import InvalidRgbaFrame, encode_rgba8_png

logger = logging.getLogger(__name__)


async def _run_one(queue: RedisPreviewQueue, compute: ComputeClient, stop: asyncio.Event) -> None:
    settings = get_settings()
    while not stop.is_set():
        job = None
        try:
            job = await queue.claim()
            if job is None:
                continue
            payload = job.request.get("payload")
            request = job.request
            # Jobs submitted before a deploy can carry the previous, expensive
            # preview limit. Apply the bound again at execution time so they
            # cannot hold every worker slot indefinitely.
            if isinstance(payload, dict):
                request = {
                    **job.request,
                    "payload": bound_preview_payload(payload),
                }
            frame = await compute.render_map_inline(request, timeout_seconds=settings.preview_compute_timeout_seconds)
            if frame.width <= 0 or frame.height <= 0:
                raise InvalidRgbaFrame("invalid_compute_frame")
            await queue.complete(job, encode_rgba8_png(rgba=frame.rgba, width=frame.width, height=frame.height))
        except (ComputeClientError, InvalidRgbaFrame):
            if job is not None:
                await queue.retry(job)
            await asyncio.sleep(0.5)
        except PreviewQueueUnavailable:
            await asyncio.sleep(1)
        except asyncio.CancelledError:
            raise
        except Exception:
            logger.exception("preview_worker_unexpected_error")
            if job is not None:
                try:
                    await queue.retry(job)
                except PreviewQueueUnavailable:
                    pass
            await asyncio.sleep(1)


async def run() -> None:
    settings = get_settings()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for value in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(value, stop.set)
        except NotImplementedError:
            pass
    queue = RedisPreviewQueue(settings)
    compute = ComputeClient(settings)
    try:
        await queue.recover_interrupted()
        await asyncio.gather(*(_run_one(queue, compute, stop) for _ in range(settings.preview_worker_concurrency)))
    finally:
        await queue.close()


if __name__ == "__main__":
    asyncio.run(run())
