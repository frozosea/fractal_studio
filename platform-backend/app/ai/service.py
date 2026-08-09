"""Owner authorization, atomic trial reservations and streaming persistence."""
from __future__ import annotations

import asyncio
import hashlib
import json
from collections.abc import AsyncIterator
from uuid import UUID, uuid4

from fastapi import HTTPException, status
from sqlalchemy import text as sql

from app.ai.models import validate_studio_suggestion
from app.ai.provider import ProviderUnavailable, stream_completion
from app.core.config import get_settings
from app.core.db import get_engine


def sse(event: str, payload: object) -> bytes:
    return f"event: {event}\ndata: {json.dumps(payload, ensure_ascii=False, separators=(',', ':'))}\n\n".encode()


async def owned_conversation(connection, owner_id: UUID, conversation_id: UUID, *, lock: bool = False):
    suffix = " FOR UPDATE" if lock else ""
    row = (await connection.execute(sql(
        "SELECT id, title, optimization_consent, created_at, updated_at FROM ai_conversations "
        "WHERE id=:id AND owner_id=:owner" + suffix
    ), {"id": conversation_id, "owner": owner_id})).mappings().one_or_none()
    if row is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="ai_conversation_not_found")
    return row


async def reserve_request(*, owner_id: UUID, conversation_id: UUID, idempotency_key: str,
                          user_text: str, request_hash: str) -> tuple[UUID, UUID, list[dict], dict | None]:
    settings = get_settings()
    if not 1 <= len(idempotency_key) <= 200:
        raise HTTPException(status_code=422, detail="invalid_idempotency_key")
    async with get_engine().begin() as connection:
        lock_key = int.from_bytes(owner_id.bytes[:8], "big", signed=True)
        await connection.execute(
            sql("SELECT pg_advisory_xact_lock(:lock_key)"), {"lock_key": lock_key}
        )
        await owned_conversation(connection, owner_id, conversation_id, lock=True)
        prior = (await connection.execute(sql(
            "SELECT r.id AS request_id,r.status,r.conversation_id,r.user_message_id,r.request_hash, "
            "m.id AS assistant_id,m.content,m.suggestion,m.created_at FROM ai_requests r "
            "LEFT JOIN ai_messages m ON m.id=r.assistant_message_id "
            "WHERE r.owner_id=:owner AND r.idempotency_key=:key"
        ), {"owner": owner_id, "key": idempotency_key})).mappings().one_or_none()
        if prior:
            if prior["request_hash"] != request_hash or prior["conversation_id"] != conversation_id:
                raise HTTPException(status_code=409, detail="idempotency_conflict")
            if prior["status"] == "completed" and prior["assistant_id"]:
                replay = dict(prior)
                replay["id"] = replay.pop("assistant_id")
                return prior["request_id"], prior["user_message_id"], [], replay
            if prior["status"] not in {"released", "failed"}:
                raise HTTPException(status_code=409, detail="idempotency_in_progress")
        member = bool(await connection.scalar(sql(
            "SELECT 1 FROM memberships WHERE user_id=:owner AND status='active'"
        ), {"owner": owner_id}))
        active = int(await connection.scalar(sql(
            "SELECT count(*) FROM ai_requests WHERE owner_id=:owner AND status IN ('reserved','streaming')"
        ), {"owner": owner_id}) or 0)
        if active >= settings.ai_max_concurrent_per_user:
            raise HTTPException(status_code=429, detail="ai_concurrency_exhausted")
        if not member:
            used = int(await connection.scalar(sql(
                "SELECT count(*) FROM ai_requests WHERE owner_id=:owner AND status='completed'"
            ), {"owner": owner_id}) or 0)
            if used + active >= settings.ai_free_lifetime_limit:
                raise HTTPException(status_code=402, detail="AI_TRIAL_EXHAUSTED")
        history_rows = (await connection.execute(sql(
            "SELECT role, content FROM ai_messages WHERE conversation_id=:cid "
            "AND created_at >= now() - make_interval(days => :days) ORDER BY created_at DESC LIMIT 20"
        ), {"cid": conversation_id, "days": settings.ai_history_ttl_days})).mappings().all()
        history = [{"role": row["role"], "content": row["content"]} for row in reversed(history_rows)]
        if prior:
            request_id, user_message_id = prior["request_id"], prior["user_message_id"]
            await connection.execute(sql(
                "UPDATE ai_requests SET status='reserved',first_output_at=NULL,completed_at=NULL WHERE id=:id"
            ), {"id": request_id})
        else:
            request_id, user_message_id = uuid4(), uuid4()
            await connection.execute(sql(
                "INSERT INTO ai_messages(id,conversation_id,role,content) VALUES(:id,:cid,'user',:content)"
            ), {"id": user_message_id, "cid": conversation_id, "content": user_text})
            await connection.execute(sql(
                "INSERT INTO ai_requests(id,owner_id,conversation_id,user_message_id,idempotency_key,status,request_hash) "
                "VALUES(:id,:owner,:cid,:mid,:key,'reserved',:request_hash)"
            ), {"id": request_id, "owner": owner_id, "cid": conversation_id, "mid": user_message_id,
                "key": idempotency_key, "request_hash": request_hash})
        await connection.execute(sql("UPDATE ai_conversations SET updated_at=now() WHERE id=:cid"), {"cid": conversation_id})
    return request_id, user_message_id, history, None


async def _mark_started(request_id: UUID) -> None:
    async with get_engine().begin() as c:
        await c.execute(sql("UPDATE ai_requests SET status='streaming', first_output_at=COALESCE(first_output_at,now()) WHERE id=:id"), {"id": request_id})


async def _finish(request_id: UUID, conversation_id: UUID, content: str, suggestion: dict | None) -> UUID:
    message_id = uuid4()
    async with get_engine().begin() as c:
        await c.execute(sql(
            "INSERT INTO ai_messages(id,conversation_id,role,content,suggestion) "
            "VALUES(:id,:cid,'assistant',:content,CAST(:suggestion AS jsonb))"
        ), {"id": message_id, "cid": conversation_id, "content": content,
            "suggestion": json.dumps(suggestion) if suggestion else None})
        await c.execute(sql(
            "UPDATE ai_requests SET status='completed', assistant_message_id=:mid, completed_at=now(), "
            "first_output_at=COALESCE(first_output_at,now()) WHERE id=:id"
        ), {"id": request_id, "mid": message_id})
    return message_id


async def _release(request_id: UUID) -> None:
    async with get_engine().begin() as c:
        await c.execute(sql("UPDATE ai_requests SET status='released', completed_at=now() WHERE id=:id AND status='reserved'"), {"id": request_id})


async def stream_message(*, owner_id: UUID, conversation_id: UUID, idempotency_key: str,
                         user_text: str, context: dict, image: bytes | None,
                         image_type: str | None, force_patch: bool = False) -> AsyncIterator[bytes]:
    fingerprint = hashlib.sha256(
        user_text.encode() + b"\0" + json.dumps(context, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False).encode() + b"\0" + (image or b"") + b"\0" + str(force_patch).encode()
    ).hexdigest()
    request_id, user_message_id, history, replay = await reserve_request(
        owner_id=owner_id, conversation_id=conversation_id, idempotency_key=idempotency_key,
        user_text=user_text, request_hash=fingerprint,
    )
    if replay:
        yield sse("message", {"id": str(replay["id"]), "role": "assistant", "replayed": True})
        if replay["content"]:
            yield sse("delta", {"content": replay["content"]})
        if replay["suggestion"]:
            yield sse("suggestion", replay["suggestion"])
        yield sse("done", {"messageId": str(replay["id"]), "replayed": True})
        return
    yield sse("message", {"id": str(user_message_id), "role": "user"})
    output: list[str] = []
    suggestion: dict | None = None
    started = False
    usage: object | None = None
    try:
        for attempt in range(2):
            try:
                phase_history = history
                if force_patch and image:
                    analysis_parts: list[str] = []
                    async for event, payload in stream_completion(
                        text=("只根据附图写 80–150 字纯视觉观察，描述可见形状、纹理、色彩和"
                              "明暗层次。禁止提出修改、输出参数或 JSON；禁止提及 variant、"
                              "公式、坐标、scale、cardioid、bulb、周期、迭代、逃逸或任何"
                              "数学身份。即使上下文提供了 spec，也不能用它替图片作视觉结论。"),
                        history=history, context=context, image=image, image_type=image_type,
                        disable_tools=True,
                    ):
                        if event == "delta" and payload:
                            if not started:
                                await _mark_started(request_id)
                                started = True
                            value = str(payload)
                            analysis_parts.append(value)
                            output.append(value)
                            yield sse("delta", {"content": value})
                        elif event == "usage":
                            usage = payload
                    phase_history = [
                        *history,
                        {"role": "user", "content": user_text},
                        {"role": "assistant", "content": "".join(analysis_parts)},
                    ]
                async for event, payload in stream_completion(
                    text=("基于上面的图像分析，只调用 propose_studio_patch 返回最小必要差异。"
                          if force_patch and image else user_text),
                    history=phase_history, context=context, image=image,
                    image_type=image_type, force_patch=force_patch
                ):
                    if event == "delta" and payload:
                        if not started:
                            await _mark_started(request_id)
                            started = True
                        output.append(str(payload))
                        yield sse("delta", {"content": str(payload)})
                    elif event == "suggestion":
                        checked = validate_studio_suggestion(payload, context)
                        if checked:
                            if not started:
                                await _mark_started(request_id)
                                started = True
                            suggestion = checked
                            yield sse("suggestion", checked)
                    elif event == "usage":
                        usage = payload
                break
            except ProviderUnavailable as error:
                if started or attempt == 1 or not error.retryable:
                    raise
        if not started:
            # A successful empty provider response is not billable and not useful.
            raise ProviderUnavailable("empty provider response")
        persisted_content = "".join(output) or str((suggestion or {}).get("reason", ""))
        message_id = await _finish(request_id, conversation_id, persisted_content, suggestion)
        if usage:
            yield sse("usage", usage)
        yield sse("done", {"messageId": str(message_id)})
    except (ProviderUnavailable, asyncio.CancelledError) as error:
        if started:
            persisted_content = "".join(output) or str((suggestion or {}).get("reason", ""))
            message_id = await asyncio.shield(_finish(request_id, conversation_id, persisted_content, suggestion))
            if not isinstance(error, asyncio.CancelledError):
                yield sse("error", {"code": "AI_PROVIDER_UNAVAILABLE", "messageId": str(message_id)})
                yield sse("done", {"messageId": str(message_id), "partial": True})
        else:
            await asyncio.shield(_release(request_id))
            if not isinstance(error, asyncio.CancelledError):
                yield sse("error", {"code": "AI_PROVIDER_UNAVAILABLE"})
        if isinstance(error, asyncio.CancelledError):
            raise


async def allowance(owner_id: UUID) -> dict[str, object]:
    settings = get_settings()
    async with get_engine().connect() as c:
        member = bool(await c.scalar(sql("SELECT 1 FROM memberships WHERE user_id=:owner AND status='active'"), {"owner": owner_id}))
        used = int(await c.scalar(sql("SELECT count(*) FROM ai_requests WHERE owner_id=:owner AND status='completed'"), {"owner": owner_id}) or 0)
    return {"member": member, "limit": None if member else settings.ai_free_lifetime_limit,
            "used": used, "remaining": None if member else max(0, settings.ai_free_lifetime_limit-used)}
