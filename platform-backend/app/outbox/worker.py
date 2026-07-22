from __future__ import annotations

import asyncio
import logging
import signal
import uuid

from app.core.config import get_settings
from app.core.db import SessionFactory
from app.infrastructure.compute.compute_client import ComputeClient
from app.studio.render_worker import RenderWorker

from .repository import ClaimedEvent, OutboxRepository


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
log = logging.getLogger("outbox-worker")


class Worker:
    def __init__(self) -> None:
        self.settings = get_settings()
        self.repository = OutboxRepository()
        self.compute = ComputeClient(self.settings)
        self.render = RenderWorker(self.settings, self.compute)
        self.stopping = asyncio.Event()

    async def poll_once(self) -> int:
        async with SessionFactory() as session:
            async with session.begin():
                events = await self.repository.claim_due_batch(
                    session,
                    limit=self.settings.outbox_batch_size,
                    lease_seconds=self.settings.outbox_lease_seconds,
                )
        for event in events:
            await self._process(event)
        return len(events)

    async def _process(self, event: ClaimedEvent) -> None:
        try:
            async with SessionFactory() as session:
                async with session.begin():
                    if event.event_type == "render.created":
                        await self.render.submit(session, event.aggregate_id)
                    elif event.event_type == "render.poll":
                        await self.render.poll(
                            session, event.aggregate_id, int(event.payload.get("sequence", 0))
                        )
                    elif event.event_type == "render.cancel_requested":
                        await self.render.cancel(session, event.aggregate_id)
                    else:
                        raise RuntimeError(f"unknown outbox event: {event.event_type}")
                    await self.repository.mark_done(session, event.id)
        except Exception as error:
            log.exception("outbox event failed", extra={"event_id": str(event.id)})
            async with SessionFactory() as session:
                async with session.begin():
                    await self.repository.reschedule(session, event.id, str(error), event.attempt_count)

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        for name in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(name, self.stopping.set)
        try:
            while not self.stopping.is_set():
                count = await self.poll_once()
                if count == 0:
                    try:
                        await asyncio.wait_for(
                            self.stopping.wait(), timeout=self.settings.outbox_poll_seconds
                        )
                    except asyncio.TimeoutError:
                        pass
        finally:
            await self.compute.close()


def main() -> None:
    asyncio.run(Worker().run())


if __name__ == "__main__":
    main()

