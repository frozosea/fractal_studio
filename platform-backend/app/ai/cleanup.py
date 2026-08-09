"""Bounded AI conversation retention work run by the existing worker."""
from __future__ import annotations

from sqlalchemy import text

from app.core.config import get_settings


async def delete_expired(connection, *, batch_size: int = 1000) -> int:
    result = await connection.execute(
        text(
            "WITH expired AS ("
            " SELECT id FROM ai_conversations "
            " WHERE updated_at < now()-make_interval(days=>:days) "
            " ORDER BY updated_at LIMIT :limit FOR UPDATE SKIP LOCKED"
            ") DELETE FROM ai_conversations WHERE id IN (SELECT id FROM expired)"
        ),
        {"days": get_settings().ai_history_ttl_days, "limit": batch_size},
    )
    return result.rowcount or 0


class AiHistoryExpiryScheduler:
    async def schedule_due_work(self, service) -> int:
        return await delete_expired(service.connection)
