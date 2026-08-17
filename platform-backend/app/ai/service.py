"""Owner authorization, atomic trial reservations and streaming persistence."""
from __future__ import annotations

import asyncio
import logging
import hashlib
import json
from collections.abc import AsyncIterator
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from uuid import UUID, uuid4

from fastapi import HTTPException, status
from sqlalchemy import text as sql

from app.ai.exploration import validate_candidate_set
from app.ai.models import validate_studio_suggestion
from app.ai.provider import AssistantMode, ProviderUnavailable, stream_completion
from app.core.config import get_settings
from app.core.db import get_engine
from app.core.logging import log_event


_ACTIVE_STATUSES = "'reserved','streaming','retrying'"
_TRIAL_STATUSES = "'reserved','streaming','retrying','partial','completed'"
_REQUEST_LEASE_SECONDS = 180


class AttemptLost(RuntimeError):
    """The worker no longer owns this request attempt."""


@dataclass(frozen=True, slots=True)
class RequestReservation:
    request_id: UUID
    user_message_id: UUID
    history: list[dict]
    replay: dict | None
    assistant_message_id: UUID
    attempt_started_at: datetime
    retrying_partial: bool = False


@dataclass(frozen=True, slots=True)
class PersistedResult:
    message_id: UUID
    status: str


@dataclass(frozen=True, slots=True)
class SettledAttempt:
    status: str | None
    message_id: UUID | None


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


async def lock_ai_owner(connection, owner_id: UUID) -> None:
    """Serialize quota, persistence and destructive work for one owner."""

    lock_key = int.from_bytes(owner_id.bytes[:8], "big", signed=True)
    await connection.execute(
        sql("SELECT pg_advisory_xact_lock(:lock_key)"), {"lock_key": lock_key}
    )


async def recompute_conversation_optimization_consent(
    connection, conversation_id: UUID, *, touch_updated_at: bool = False
) -> None:
    """Keep the conversation flag a projection of explicit feedback consent."""

    updated_at = "updated_at=now()," if touch_updated_at else ""
    await connection.execute(sql(
        f"UPDATE ai_conversations SET {updated_at}optimization_consent=EXISTS("
        " SELECT 1 FROM ai_feedback f JOIN ai_messages m ON m.id=f.message_id"
        " WHERE m.conversation_id=:cid AND f.consent"
        ") WHERE id=:cid"
    ), {"cid": conversation_id})


async def withdraw_conversation_optimization_consent(
    connection, conversation_id: UUID
) -> None:
    """Atomically opt out every feedback row before recomputing the projection."""

    await connection.execute(sql(
        "UPDATE ai_feedback f SET consent=false FROM ai_messages m "
        "WHERE m.id=f.message_id AND m.conversation_id=:cid AND f.consent"
    ), {"cid": conversation_id})
    await recompute_conversation_optimization_consent(connection, conversation_id)


async def recover_expired_requests(connection, *, owner_id: UUID | None = None) -> int:
    """Release abandoned reservations and retain abandoned visible output."""

    owner_predicate = " AND owner_id=:owner" if owner_id is not None else ""
    parameters = {"owner": owner_id} if owner_id is not None else {}
    result = await connection.execute(sql(
        "UPDATE ai_requests SET "
        "status=CASE WHEN first_output_at IS NULL THEN 'released' ELSE 'partial' END,"
        "completed_at=now(),lease_until=NULL "
        f"WHERE status IN ({_ACTIVE_STATUSES}) "
        "AND (lease_until IS NULL OR lease_until<=now())" + owner_predicate
    ), parameters)
    return result.rowcount or 0


async def ensure_no_active_requests(
    connection, *, owner_id: UUID, conversation_id: UUID | None = None
) -> None:
    conversation_predicate = " AND conversation_id=:cid" if conversation_id else ""
    parameters: dict[str, object] = {"owner": owner_id}
    if conversation_id:
        parameters["cid"] = conversation_id
    active = await connection.scalar(sql(
        "SELECT 1 FROM ai_requests WHERE owner_id=:owner "
        f"AND status IN ({_ACTIVE_STATUSES})" + conversation_predicate + " LIMIT 1"
    ), parameters)
    if active:
        raise HTTPException(status_code=409, detail="ai_request_in_progress")


async def scrub_conversation_ledger(
    connection, *, owner_id: UUID, conversation_id: UUID | None = None
) -> int:
    """Detach retained billing facts from deleted user content and identifiers."""

    if conversation_id is None:
        predicate = (
            "conversation_id IN (SELECT id FROM ai_conversations WHERE owner_id=:owner)"
        )
        parameters: dict[str, object] = {"owner": owner_id}
    else:
        predicate = "conversation_id=:cid"
        parameters = {"owner": owner_id, "cid": conversation_id}
    result = await connection.execute(sql(
        "UPDATE ai_requests SET conversation_id=NULL,user_message_id=NULL,"
        "assistant_message_id=NULL,idempotency_key=NULL,request_hash=NULL,"
        "provider_request_id=NULL WHERE owner_id=:owner AND " + predicate
    ), parameters)
    return result.rowcount or 0


async def reserve_request(*, owner_id: UUID, conversation_id: UUID, idempotency_key: str,
                          user_text: str, request_hash: str) -> RequestReservation:
    settings = get_settings()
    if not 1 <= len(idempotency_key) <= 200:
        raise HTTPException(status_code=422, detail="invalid_idempotency_key")
    async with get_engine().begin() as connection:
        await lock_ai_owner(connection, owner_id)
        await recover_expired_requests(connection, owner_id=owner_id)
        await owned_conversation(connection, owner_id, conversation_id, lock=True)
        prior = (await connection.execute(sql(
            "SELECT r.id AS request_id,r.status,r.conversation_id,r.user_message_id,r.request_hash, "
            "r.first_output_at,r.assistant_message_id,r.counts_toward_trial,"
            "m.id AS persisted_assistant_id,m.content,m.suggestion,m.created_at "
            "FROM ai_requests r "
            "LEFT JOIN ai_messages m ON m.id=r.assistant_message_id "
            "WHERE r.owner_id=:owner AND r.idempotency_key=:key"
        ), {"owner": owner_id, "key": idempotency_key})).mappings().one_or_none()
        if prior:
            if prior["request_hash"] != request_hash or prior["conversation_id"] != conversation_id:
                raise HTTPException(status_code=409, detail="idempotency_conflict")
            if prior["status"] == "completed" and prior["persisted_assistant_id"]:
                replay = dict(prior)
                replay["id"] = replay["persisted_assistant_id"]
                return RequestReservation(
                    request_id=prior["request_id"],
                    user_message_id=prior["user_message_id"],
                    history=[],
                    replay=replay,
                    assistant_message_id=replay["id"],
                    attempt_started_at=datetime.now(timezone.utc),
                )
            if prior["status"] == "completed":
                raise HTTPException(status_code=409, detail="idempotency_conflict")
            if prior["status"] not in {"released", "failed", "partial"}:
                raise HTTPException(status_code=409, detail="idempotency_in_progress")
            if prior["user_message_id"] is None:
                raise HTTPException(status_code=409, detail="idempotency_conflict")
        member = bool(await connection.scalar(sql(
            "SELECT 1 FROM memberships WHERE user_id=:owner AND status='active'"
        ), {"owner": owner_id}))
        counts = (await connection.execute(sql(
            "SELECT "
            f"count(*) FILTER (WHERE status IN ({_ACTIVE_STATUSES})) AS active,"
            "count(*) FILTER (WHERE counts_toward_trial "
            f"AND status IN ({_TRIAL_STATUSES})) AS trial_used "
            "FROM ai_requests WHERE owner_id=:owner"
        ), {"owner": owner_id})).mappings().one()
        active = int(counts["active"] or 0)
        trial_used = int(counts["trial_used"] or 0)
        if active >= settings.ai_max_concurrent_per_user:
            raise HTTPException(status_code=429, detail="ai_concurrency_exhausted")
        retrying_partial = bool(prior and prior["status"] == "partial")
        counts_toward_trial = (
            bool(prior["counts_toward_trial"])
            if retrying_partial
            else not member
        )
        if (
            counts_toward_trial
            and not retrying_partial
            and trial_used >= settings.ai_free_lifetime_limit
        ):
            raise HTTPException(status_code=402, detail="AI_TRIAL_EXHAUSTED")
        history_parameters: dict[str, object] = {
            "cid": conversation_id,
            "days": settings.ai_history_ttl_days,
        }
        history_exclusions = ""
        if prior:
            history_exclusions += " AND id<>:user_message_id"
            history_parameters["user_message_id"] = prior["user_message_id"]
            if prior["assistant_message_id"]:
                history_exclusions += " AND id<>:assistant_message_id"
                history_parameters["assistant_message_id"] = prior["assistant_message_id"]
        history_rows = (await connection.execute(sql(
            "SELECT role, content FROM ai_messages WHERE conversation_id=:cid "
            "AND created_at >= now() - make_interval(days => :days)" + history_exclusions +
            " ORDER BY created_at DESC LIMIT 20"
        ), history_parameters)).mappings().all()
        history = [{"role": row["role"], "content": row["content"]} for row in reversed(history_rows)]
        attempt_started_at = datetime.now(timezone.utc)
        lease_until = attempt_started_at + timedelta(seconds=_REQUEST_LEASE_SECONDS)
        if prior:
            request_id, user_message_id = prior["request_id"], prior["user_message_id"]
            assistant_message_id = prior["assistant_message_id"] or uuid4()
            if retrying_partial:
                update = await connection.execute(sql(
                    "UPDATE ai_requests SET status='retrying',completed_at=NULL,"
                    "attempt_started_at=:attempt,lease_until=:lease,"
                    "assistant_message_id=:assistant_id "
                    "WHERE id=:id AND owner_id=:owner AND status='partial' RETURNING id"
                ), {"id": request_id, "owner": owner_id, "attempt": attempt_started_at,
                    "lease": lease_until, "assistant_id": assistant_message_id})
            else:
                update = await connection.execute(sql(
                    "UPDATE ai_requests SET status='reserved',first_output_at=NULL,completed_at=NULL,"
                    "attempt_started_at=:attempt,lease_until=:lease,"
                    "assistant_message_id=:assistant_id,counts_toward_trial=:counts "
                    "WHERE id=:id AND owner_id=:owner AND status IN ('released','failed') RETURNING id"
                ), {"id": request_id, "owner": owner_id, "attempt": attempt_started_at,
                    "lease": lease_until, "assistant_id": assistant_message_id,
                    "counts": counts_toward_trial})
            if update.scalar_one_or_none() is None:
                raise HTTPException(status_code=409, detail="idempotency_in_progress")
        else:
            request_id, user_message_id, assistant_message_id = uuid4(), uuid4(), uuid4()
            await connection.execute(sql(
                "INSERT INTO ai_messages(id,conversation_id,role,content) VALUES(:id,:cid,'user',:content)"
            ), {"id": user_message_id, "cid": conversation_id, "content": user_text})
            await connection.execute(sql(
                "INSERT INTO ai_requests(id,owner_id,conversation_id,user_message_id,"
                "assistant_message_id,idempotency_key,status,request_hash,counts_toward_trial,"
                "attempt_started_at,lease_until) "
                "VALUES(:id,:owner,:cid,:mid,:assistant_id,:key,'reserved',:request_hash,"
                ":counts,:attempt,:lease)"
            ), {"id": request_id, "owner": owner_id, "cid": conversation_id, "mid": user_message_id,
                "assistant_id": assistant_message_id, "key": idempotency_key,
                "request_hash": request_hash, "counts": counts_toward_trial,
                "attempt": attempt_started_at, "lease": lease_until})
        await connection.execute(sql("UPDATE ai_conversations SET updated_at=now() WHERE id=:cid"), {"cid": conversation_id})
    return RequestReservation(
        request_id=request_id,
        user_message_id=user_message_id,
        history=history,
        replay=None,
        assistant_message_id=assistant_message_id,
        attempt_started_at=attempt_started_at,
        retrying_partial=retrying_partial,
    )


async def _mark_started(request_id: UUID, attempt_started_at: datetime) -> None:
    async with get_engine().begin() as c:
        result = await c.execute(sql(
            "UPDATE ai_requests SET status=CASE WHEN status='retrying' THEN 'retrying' "
            "ELSE 'streaming' END,first_output_at=COALESCE(first_output_at,now()),"
            "lease_until=now()+make_interval(secs=>:lease_seconds) "
            "WHERE id=:id AND attempt_started_at=:attempt "
            "AND status IN ('reserved','retrying') RETURNING id"
        ), {"id": request_id, "attempt": attempt_started_at,
            "lease_seconds": _REQUEST_LEASE_SECONDS})
        if result.scalar_one_or_none() is None:
            raise AttemptLost("AI request attempt lease was lost before first output")


async def _refresh_lease(request_id: UUID, attempt_started_at: datetime) -> None:
    """Keep a non-billable provider attempt from being reclaimed mid-call."""

    async with get_engine().begin() as c:
        result = await c.execute(sql(
            "UPDATE ai_requests SET lease_until=now()+make_interval(secs=>:lease_seconds) "
            "WHERE id=:id AND attempt_started_at=:attempt "
            "AND status IN ('reserved','retrying') RETURNING id"
        ), {"id": request_id, "attempt": attempt_started_at,
            "lease_seconds": _REQUEST_LEASE_SECONDS})
        if result.scalar_one_or_none() is None:
            raise AttemptLost("AI request attempt lease was lost")


async def _persist_result(*, request_id: UUID, conversation_id: UUID, content: str,
                          suggestion: dict | None, final_status: str,
                          assistant_message_id: UUID,
                          attempt_started_at: datetime) -> PersistedResult:
    if final_status not in {"completed", "partial"}:
        raise ValueError("invalid AI request final status")
    suggestion_json = json.dumps(suggestion) if suggestion else None
    async with get_engine().begin() as c:
        owner_id = await c.scalar(sql(
            "SELECT owner_id FROM ai_requests WHERE id=:id"
        ), {"id": request_id})
        if owner_id is None:
            raise AttemptLost("AI request no longer exists")
        await lock_ai_owner(c, owner_id)
        row = (await c.execute(sql(
            "SELECT status,conversation_id,assistant_message_id,attempt_started_at "
            "FROM ai_requests WHERE id=:id AND owner_id=:owner FOR UPDATE"
        ), {"id": request_id, "owner": owner_id})).mappings().one_or_none()
        if row is None:
            raise AttemptLost("AI request no longer exists")
        if row["status"] == "completed":
            if row["assistant_message_id"] is None:
                raise AttemptLost("completed AI request is missing its message identifier")
            return PersistedResult(row["assistant_message_id"], "completed")
        if (
            row["conversation_id"] != conversation_id
            or row["assistant_message_id"] != assistant_message_id
            or row["attempt_started_at"] != attempt_started_at
            or row["status"] not in {"streaming", "retrying"}
        ):
            raise AttemptLost("AI request attempt no longer owns persistence")
        conversation_exists = await c.scalar(sql(
            "SELECT 1 FROM ai_conversations WHERE id=:cid AND owner_id=:owner FOR UPDATE"
        ), {"cid": conversation_id, "owner": owner_id})
        if not conversation_exists:
            raise AttemptLost("AI conversation was deleted")

        # Retrying a partial replaces the same message.  Feedback on the old
        # text cannot remain attached to the replacement text.
        await c.execute(sql(
            "DELETE FROM ai_feedback WHERE message_id=:mid"
        ), {"mid": assistant_message_id})
        inserted = await c.execute(sql(
            "INSERT INTO ai_messages(id,conversation_id,role,content,suggestion) "
            "VALUES(:id,:cid,'assistant',:content,CAST(:suggestion AS jsonb)) "
            "ON CONFLICT(id) DO UPDATE SET content=excluded.content,suggestion=excluded.suggestion "
            "WHERE ai_messages.conversation_id=excluded.conversation_id "
            "AND ai_messages.role='assistant' RETURNING id"
        ), {"id": assistant_message_id, "cid": conversation_id, "content": content,
            "suggestion": suggestion_json})
        if inserted.scalar_one_or_none() is None:
            raise AttemptLost("stable assistant message identifier collided")
        updated = await c.execute(sql(
            "UPDATE ai_requests SET status=:status,completed_at=now(),lease_until=NULL,"
            "first_output_at=COALESCE(first_output_at,now()) "
            "WHERE id=:id AND conversation_id=:cid AND assistant_message_id=:mid "
            "AND attempt_started_at=:attempt AND status IN ('streaming','retrying') "
            "RETURNING id"
        ), {"id": request_id, "cid": conversation_id, "mid": assistant_message_id,
            "attempt": attempt_started_at, "status": final_status})
        if updated.scalar_one_or_none() is None:
            raise AttemptLost("AI request attempt lost its terminal compare-and-set")
        await recompute_conversation_optimization_consent(
            c, conversation_id, touch_updated_at=True
        )
    return PersistedResult(assistant_message_id, final_status)


async def _settle_attempt(
    request_id: UUID, attempt_started_at: datetime
) -> SettledAttempt:
    """CAS an unfinished attempt to released/partial after any exception."""

    async with get_engine().begin() as c:
        owner_id = await c.scalar(sql(
            "SELECT owner_id FROM ai_requests WHERE id=:id"
        ), {"id": request_id})
        if owner_id is None:
            return SettledAttempt(None, None)
        await lock_ai_owner(c, owner_id)
        row = (await c.execute(sql(
            "SELECT status,first_output_at,assistant_message_id,attempt_started_at "
            "FROM ai_requests WHERE id=:id AND owner_id=:owner FOR UPDATE"
        ), {"id": request_id, "owner": owner_id})).mappings().one_or_none()
        if row is None:
            return SettledAttempt(None, None)
        if row["status"] not in {"reserved", "streaming", "retrying"}:
            return SettledAttempt(row["status"], row["assistant_message_id"])
        if row["attempt_started_at"] != attempt_started_at:
            return SettledAttempt(row["status"], row["assistant_message_id"])
        final_status = "partial" if row["first_output_at"] is not None else "released"
        updated = await c.execute(sql(
            "UPDATE ai_requests SET status=:status,completed_at=now(),lease_until=NULL "
            "WHERE id=:id AND attempt_started_at=:attempt "
            "AND status IN ('reserved','streaming','retrying') RETURNING id"
        ), {"id": request_id, "attempt": attempt_started_at, "status": final_status})
        if updated.scalar_one_or_none() is None:
            return SettledAttempt(None, None)
        return SettledAttempt(final_status, row["assistant_message_id"])


async def stream_message(*, owner_id: UUID, conversation_id: UUID, idempotency_key: str,
                         user_text: str, context: dict, image: bytes | None,
                         image_type: str | None, force_patch: bool = False,
                         assistant_mode: AssistantMode = "chat") -> AsyncIterator[bytes]:
    fingerprint_context = {
        key: value for key, value in context.items() if key not in {"member", "capabilities"}
    }
    fingerprint = hashlib.sha256(
        user_text.encode() + b"\0" + json.dumps(
            fingerprint_context, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode() + b"\0" + (image or b"") + b"\0" + str(force_patch).encode()
        + b"\0" + assistant_mode.encode()
    ).hexdigest()
    reservation = await reserve_request(
        owner_id=owner_id, conversation_id=conversation_id, idempotency_key=idempotency_key,
        user_text=user_text, request_hash=fingerprint,
    )
    if reservation.replay:
        replay = reservation.replay
        yield sse("message", {"id": str(replay["id"]), "role": "assistant", "replayed": True})
        if replay["content"]:
            yield sse("delta", {"content": replay["content"]})
        if replay["suggestion"]:
            yield sse("suggestion", replay["suggestion"])
        yield sse("done", {"messageId": str(replay["id"]), "replayed": True})
        return
    request_id = reservation.request_id
    history = reservation.history
    output: list[str] = []
    suggestion: dict | None = None
    started = False
    completed = False
    usage: object | None = None
    try:
        yield sse("message", {"id": str(reservation.user_message_id), "role": "user"})
        for attempt in range(2):
            try:
                await _refresh_lease(request_id, reservation.attempt_started_at)
                phase_history = history
                phase_image = image
                phase_image_type = image_type
                requires_suggestion = force_patch or assistant_mode != "chat"
                defer_until_suggestion = requires_suggestion and image is not None
                deferred_observation = ""
                if requires_suggestion and image:
                    analysis_parts: list[str] = []
                    async for event, payload in stream_completion(
                        text=("只根据附图，依次按中心、左上、右上、左下、右下、整体色彩各写一句"
                              "纯视觉观察。明确亮细节、大片暗区、相对位置、画面裁切和留白；"
                              "看不清的内容直接写不确定。禁止比喻、提出修改、输出参数或 JSON；"
                              "禁止分形名称、variant、公式、坐标、scale、cardioid、bulb、周期、"
                              "迭代、逃逸或任何数学身份。即使上下文提供了 spec，也不能用它替"
                              "图片作视觉结论。"),
                        history=history, context=context, image=image, image_type=image_type,
                        disable_tools=True, assistant_mode=assistant_mode,
                    ):
                        if event == "delta" and payload:
                            value = str(payload)
                            analysis_parts.append(value)
                        elif event == "usage":
                            usage = payload
                    deferred_observation = "".join(analysis_parts)
                    phase_history = [
                        *history,
                        {"role": "user", "content": user_text},
                        {"role": "assistant", "content": deferred_observation},
                    ]
                    # The tool phase relies on the trusted visual observation
                    # and structured context; uploading the same large image a
                    # second time only adds latency and provider failure risk.
                    phase_image = None
                    phase_image_type = None
                    await _refresh_lease(request_id, reservation.attempt_started_at)
                mode_instruction = {
                    "location": (
                        "基于上面的图像分析，只调用 propose_studio_patch 给出三个位置探索候选。"
                        "当前基准由界面单独显示，三个候选都必须是非零且彼此明显不同的变化。"
                        "固定使用 position 轴，三个 scaleFactor 必须精确为 1；"
                        "只返回归一化相对位移，不计算绝对坐标。缩放和旋转由构图助手负责。"
                    ),
                    "color": (
                        "基于上面的图像分析，只调用 propose_studio_patch 给出四个视觉上明显不同的调色候选。"
                        "只修改工具允许的调色字段，不改变结构和计算量。"
                    ),
                    "composition": (
                        "基于上面的图像分析，只调用 propose_studio_patch 给出三个构图候选。"
                        "当前基准由界面单独显示，三个候选都必须是非零且彼此明显不同的变化。"
                        "只返回归一化相对平移、缩放和旋转，不计算绝对坐标，不改颜色或公式。"
                        "scaleFactor<1 表示放大并收紧画面，>1 表示缩小视图并显示更多周边。"
                    ),
                }.get(assistant_mode)
                async for event, payload in stream_completion(
                    text=(mode_instruction or
                          "基于上面的图像分析，只调用 propose_studio_patch 返回最小必要差异。"
                          if requires_suggestion and image else user_text),
                    history=phase_history, context=context, image=phase_image,
                    image_type=phase_image_type, force_patch=requires_suggestion,
                    assistant_mode=assistant_mode,
                ):
                    if event == "delta" and payload:
                        if defer_until_suggestion and not started:
                            # The tool connection may still fail.  Keep all
                            # pre-tool text private so a retry remains unbilled
                            # and does not duplicate observations in the UI.
                            continue
                        if not started:
                            await _mark_started(request_id, reservation.attempt_started_at)
                            started = True
                        output.append(str(payload))
                        yield sse("delta", {"content": str(payload)})
                    elif event == "suggestion":
                        checked = (
                            validate_candidate_set(payload, context, assistant_mode)
                            if assistant_mode != "chat"
                            else validate_studio_suggestion(payload, context)
                        )
                        if checked:
                            if not started:
                                await _mark_started(request_id, reservation.attempt_started_at)
                                started = True
                                if deferred_observation:
                                    output.append(deferred_observation)
                                    yield sse("delta", {"content": deferred_observation})
                            suggestion = checked
                            yield sse("suggestion", checked)
                    elif event == "usage":
                        usage = payload
                if requires_suggestion and suggestion is None:
                    raise ProviderUnavailable("invalid provider tool response")
                break
            except ProviderUnavailable as error:
                if started or attempt == 1 or not error.retryable:
                    raise
        if not started:
            # A successful empty provider response is not billable and not useful.
            raise ProviderUnavailable("empty provider response")
        persisted_content = "".join(output) or str((suggestion or {}).get("reason", ""))
        persisted = await _persist_result(
            request_id=request_id,
            conversation_id=conversation_id,
            content=persisted_content,
            suggestion=suggestion,
            final_status="completed",
            assistant_message_id=reservation.assistant_message_id,
            attempt_started_at=reservation.attempt_started_at,
        )
        completed = True
        if usage:
            yield sse("usage", usage)
        yield sse("done", {"messageId": str(persisted.message_id)})
    except BaseException as error:
        disconnected = isinstance(error, (asyncio.CancelledError, GeneratorExit))
        if completed:
            raise
        terminal_status: str | None = None
        message_id: UUID | None = None
        if started:
            persisted_content = "".join(output) or str((suggestion or {}).get("reason", ""))
            try:
                partial = await asyncio.shield(_persist_result(
                    request_id=request_id,
                    conversation_id=conversation_id,
                    content=persisted_content,
                    suggestion=suggestion,
                    final_status="partial",
                    assistant_message_id=reservation.assistant_message_id,
                    attempt_started_at=reservation.attempt_started_at,
                ))
                terminal_status, message_id = partial.status, partial.message_id
            except BaseException:
                # The message write and request CAS share a transaction.  A
                # second CAS safely resolves an ordinary rollback or observes
                # an ambiguous commit without downgrading `completed`.
                try:
                    settled = await asyncio.shield(_settle_attempt(
                        request_id, reservation.attempt_started_at
                    ))
                    terminal_status, message_id = settled.status, settled.message_id
                except BaseException:
                    # A database outage is recovered by the request lease.  Do
                    # not replace the original provider/cancellation exception.
                    pass
        else:
            try:
                settled = await asyncio.shield(_settle_attempt(
                    request_id, reservation.attempt_started_at
                ))
                terminal_status, message_id = settled.status, settled.message_id
            except BaseException:
                pass
        if disconnected or not isinstance(error, Exception):
            raise
        # Diagnostic breadcrumb only: sanitized error kind and attempt round,
        # never the provider body, prompt, image or any credential. log_event
        # drops arbitrary metadata by design, so the safe fields go in the
        # message itself.
        if isinstance(error, ProviderUnavailable):
            log_event(
                logging.WARNING,
                f"AI provider attempt failed: {str(error)} "
                f"(retryable={error.retryable}, attempt={attempt}, terminal={terminal_status})",
            )
        error_payload: dict[str, object] = {"code": "AI_PROVIDER_UNAVAILABLE"}
        if message_id and terminal_status in {"partial", "completed"}:
            error_payload["messageId"] = str(message_id)
        yield sse("error", error_payload)
        if message_id and terminal_status in {"partial", "completed"}:
            done_payload: dict[str, object] = {
                "messageId": str(message_id),
                "partial": terminal_status == "partial",
            }
            if terminal_status == "completed":
                done_payload["recovered"] = True
            yield sse("done", done_payload)


async def allowance(owner_id: UUID) -> dict[str, object]:
    settings = get_settings()
    async with get_engine().connect() as c:
        member = bool(await c.scalar(sql("SELECT 1 FROM memberships WHERE user_id=:owner AND status='active'"), {"owner": owner_id}))
        used = int(await c.scalar(sql(
            "SELECT count(*) FROM ai_requests WHERE owner_id=:owner "
            "AND counts_toward_trial "
            "AND status IN ('reserved','streaming','retrying','partial','completed')"
        ), {"owner": owner_id}) or 0)
    return {"member": member, "limit": None if member else settings.ai_free_lifetime_limit,
            "used": used, "remaining": None if member else max(0, settings.ai_free_lifetime_limit-used)}
