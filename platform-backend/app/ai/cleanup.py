"""Bounded AI conversation retention work run by the existing worker."""
from __future__ import annotations

from fastapi import HTTPException
from sqlalchemy import text

from app.ai.service import (
    ensure_no_active_requests,
    lock_ai_owner,
    recover_expired_requests,
    scrub_conversation_ledger,
)
from app.core.config import get_settings


async def delete_expired(connection, *, batch_size: int = 1000) -> int:
    settings = get_settings()
    candidates = (await connection.execute(
        text(
            "SELECT id,owner_id FROM ai_conversations "
            " WHERE updated_at < now()-make_interval(days=>:days) "
            " ORDER BY owner_id,updated_at LIMIT :limit"
        ),
        {"days": settings.ai_history_ttl_days, "limit": batch_size},
    )).mappings().all()
    deleted = 0
    for candidate in candidates:
        owner_id = candidate["owner_id"]
        conversation_id = candidate["id"]
        await lock_ai_owner(connection, owner_id)
        await recover_expired_requests(connection, owner_id=owner_id)
        still_expired = await connection.scalar(text(
            "SELECT 1 FROM ai_conversations WHERE id=:id AND owner_id=:owner "
            "AND updated_at < now()-make_interval(days=>:days) FOR UPDATE"
        ), {"id": conversation_id, "owner": owner_id,
            "days": settings.ai_history_ttl_days})
        if not still_expired:
            continue
        try:
            await ensure_no_active_requests(
                connection, owner_id=owner_id, conversation_id=conversation_id
            )
        except HTTPException as error:
            if error.detail == "ai_request_in_progress":
                continue
            raise
        await scrub_conversation_ledger(
            connection, owner_id=owner_id, conversation_id=conversation_id
        )
        result = await connection.execute(text(
            "DELETE FROM ai_conversations WHERE id=:id AND owner_id=:owner"
        ), {"id": conversation_id, "owner": owner_id})
        deleted += result.rowcount or 0
    return deleted


async def run_maintenance(connection, *, batch_size: int = 1000) -> int:
    recovered = await recover_expired_requests(connection)
    deleted = await delete_expired(connection, batch_size=batch_size)
    return recovered + deleted


class AiHistoryExpiryScheduler:
    async def schedule_due_work(self, service) -> int:
        return await run_maintenance(service.connection)
