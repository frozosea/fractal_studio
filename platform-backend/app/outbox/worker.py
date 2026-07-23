"""At-least-once event dispatcher; domain work stays in owner modules."""

from __future__ import annotations

import asyncio


async def run() -> None:
    """Keep development worker alive until M7 claim/dispatch is implemented."""
    while True:
        await asyncio.sleep(60)


if __name__ == "__main__":
    asyncio.run(run())
