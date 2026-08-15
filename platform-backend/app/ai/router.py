"""Authenticated Studio AI conversation and streaming HTTP boundary."""
from __future__ import annotations

import json
from collections.abc import AsyncIterator
from io import BytesIO
from uuid import UUID, uuid4

from fastapi import APIRouter, Depends, File, Form, Header, HTTPException, Request, UploadFile
from fastapi.responses import JSONResponse, Response, StreamingResponse
from PIL import Image, UnidentifiedImageError
from pydantic import ValidationError
from sqlalchemy import text as sql

from app.ai.images import prepare_ai_image
from app.ai.models import (
    ConversationCreate,
    ConversationUpdate,
    FeedbackInput,
    StudioAIClientContext,
)
from app.ai.provider import AssistantMode
from app.ai.service import (
    allowance,
    ensure_no_active_requests,
    lock_ai_owner,
    owned_conversation,
    recompute_conversation_optimization_consent,
    recover_expired_requests,
    scrub_conversation_ledger,
    stream_message,
    withdraw_conversation_optimization_consent,
)
from app.auth.models import AccessPrincipal
from app.core import idempotency_service
from app.core.access_middleware import enforce_origin_and_csrf, require_principal
from app.core.config import get_settings
from app.core.db import get_engine
from app.infrastructure.compute.compute_client import ComputeClientError
from app.studio.capability_service import studio_capabilities

router = APIRouter(prefix="/v1", tags=["ai"])


def _conversation(row) -> dict[str, object]:
    return {"id": str(row["id"]), "title": row["title"],
            "optimizationConsent": row["optimization_consent"],
            "createdAt": row["created_at"].isoformat(), "updatedAt": row["updated_at"].isoformat()}


def _replay(claim) -> Response | None:
    if not claim.is_replay:
        return None
    return JSONResponse(status_code=claim.replay_status or 200, content=claim.replay_body or {},
                        headers=dict(claim.replay_headers or {}))


@router.get("/ai/conversations")
async def conversations(principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    async with get_engine().connect() as c:
        rows = (await c.execute(sql(
            "SELECT id,title,optimization_consent,created_at,updated_at FROM ai_conversations "
            "WHERE owner_id=:owner ORDER BY updated_at DESC LIMIT 100"
        ), {"owner": principal.user_id})).mappings().all()
    return {"data": [_conversation(row) for row in rows], "page": {"nextCursor": None}}


@router.post("/ai/conversations", status_code=201)
async def create_conversation(payload: ConversationCreate, request: Request,
                              idempotency_key: str = Header(..., alias="Idempotency-Key"),
                              principal: AccessPrincipal = Depends(require_principal)) -> Response:
    enforce_origin_and_csrf(request, principal)
    title = payload.title.strip()
    if not title:
        raise HTTPException(status_code=422, detail="validation_error")
    async with get_engine().begin() as c:
        claim = await idempotency_service.claim(c, user_id=principal.user_id,
            scope="ai.create_conversation", key=idempotency_key, body={"title": title})
        if response := _replay(claim):
            return response
        row = (await c.execute(sql(
            "INSERT INTO ai_conversations(id,owner_id,title) VALUES(:id,:owner,:title) "
            "RETURNING id,title,optimization_consent,created_at,updated_at"
        ), {"id": uuid4(), "owner": principal.user_id, "title": title})).mappings().one()
        body = {"data": _conversation(row)}
        await idempotency_service.complete(c, claim, response_status=201, response_body=body)
    return JSONResponse(status_code=201, content=body)


@router.patch("/ai/conversations/{conversation_id}")
async def update_conversation(conversation_id: UUID, payload: ConversationUpdate, request: Request,
                              idempotency_key: str = Header(..., alias="Idempotency-Key"),
                              principal: AccessPrincipal = Depends(require_principal)) -> Response:
    enforce_origin_and_csrf(request, principal)
    changes = payload.model_dump(exclude_unset=True, by_alias=False)
    if not changes:
        raise HTTPException(status_code=422, detail="validation_error")
    if changes.get("optimization_consent") is True:
        # Opt-in is meaningful only on an explicit thumbs-up/down. The feedback
        # endpoint owns that write and the aggregate conversation projection.
        raise HTTPException(status_code=422, detail="ai_feedback_consent_required")
    if changes.get("title") is not None:
        changes["title"] = changes["title"].strip()
        if not changes["title"]:
            raise HTTPException(status_code=422, detail="validation_error")
    async with get_engine().begin() as c:
        await owned_conversation(c, principal.user_id, conversation_id, lock=True)
        claim = await idempotency_service.claim(c, user_id=principal.user_id,
            scope=f"ai.update_conversation:{conversation_id}", key=idempotency_key, body=changes)
        if response := _replay(claim):
            return response
        if changes.get("optimization_consent") is False:
            await withdraw_conversation_optimization_consent(c, conversation_id)
        row = (await c.execute(sql(
            "UPDATE ai_conversations SET title=COALESCE(:title,title), updated_at=now() "
            "WHERE id=:id RETURNING id,title,optimization_consent,created_at,updated_at"
        ), {"id": conversation_id, "title": changes.get("title")})).mappings().one()
        body = {"data": _conversation(row)}
        await idempotency_service.complete(c, claim, response_status=200, response_body=body)
    return JSONResponse(content=body)


@router.delete("/ai/conversations/{conversation_id}", status_code=204)
async def delete_conversation(conversation_id: UUID, request: Request,
                              idempotency_key: str = Header(..., alias="Idempotency-Key"),
                              principal: AccessPrincipal = Depends(require_principal)) -> Response:
    enforce_origin_and_csrf(request, principal)
    async with get_engine().begin() as c:
        await lock_ai_owner(c, principal.user_id)
        await recover_expired_requests(c, owner_id=principal.user_id)
        await owned_conversation(c, principal.user_id, conversation_id, lock=True)
        claim = await idempotency_service.claim(c, user_id=principal.user_id,
            scope=f"ai.delete_conversation:{conversation_id}", key=idempotency_key,
            body={"id": str(conversation_id)})
        if response := _replay(claim):
            return response
        await ensure_no_active_requests(
            c, owner_id=principal.user_id, conversation_id=conversation_id
        )
        await scrub_conversation_ledger(
            c, owner_id=principal.user_id, conversation_id=conversation_id
        )
        await c.execute(sql("DELETE FROM ai_conversations WHERE id=:id"), {"id": conversation_id})
        await idempotency_service.complete(c, claim, response_status=204, response_body=None)
    return Response(status_code=204)


@router.get("/ai/conversations/{conversation_id}/messages")
async def messages(conversation_id: UUID,
                   principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    async with get_engine().connect() as c:
        await owned_conversation(c, principal.user_id, conversation_id)
        rows = (await c.execute(sql(
            "SELECT m.id,m.conversation_id,m.role,m.content,m.suggestion,m.created_at,f.rating,"
            "r.status AS request_status "
            "FROM ai_messages m LEFT JOIN ai_feedback f ON f.message_id=m.id "
            "LEFT JOIN ai_requests r ON r.assistant_message_id=m.id "
            "WHERE m.conversation_id=:id ORDER BY m.created_at LIMIT 500"
        ), {"id": conversation_id})).mappings().all()
    return {"data": [{"id": str(row["id"]), "conversationId": str(row["conversation_id"]),
                       "role": row["role"], "content": row["content"], "suggestion": row["suggestion"],
                       "feedback": row["rating"], "status": row["request_status"],
                       "createdAt": row["created_at"].isoformat()} for row in rows],
            "page": {"nextCursor": None}}


@router.delete("/me/ai-conversations", status_code=204)
async def delete_all(request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
                     principal: AccessPrincipal = Depends(require_principal)) -> Response:
    enforce_origin_and_csrf(request, principal)
    async with get_engine().begin() as c:
        await lock_ai_owner(c, principal.user_id)
        await recover_expired_requests(c, owner_id=principal.user_id)
        claim = await idempotency_service.claim(c, user_id=principal.user_id,
            scope="ai.delete_all_conversations", key=idempotency_key, body={})
        if response := _replay(claim):
            return response
        await ensure_no_active_requests(c, owner_id=principal.user_id)
        await scrub_conversation_ledger(c, owner_id=principal.user_id)
        await c.execute(sql("DELETE FROM ai_conversations WHERE owner_id=:owner"), {"owner": principal.user_id})
        await idempotency_service.complete(c, claim, response_status=204, response_body=None)
    return Response(status_code=204)


def _reject_json_constant(value: str):
    raise ValueError(f"non-finite JSON number: {value}")


def _parse_context(raw: str) -> dict:
    if len(raw.encode()) > 100_000:
        raise HTTPException(status_code=413, detail="payload_too_large")
    try:
        value = json.loads(raw, parse_constant=_reject_json_constant)
    except (json.JSONDecodeError, ValueError) as error:
        raise HTTPException(status_code=422, detail="validation_error") from error
    if not isinstance(value, dict):
        raise HTTPException(status_code=422, detail="validation_error")
    try:
        parsed = StudioAIClientContext.model_validate(value)
    except ValidationError as error:
        raise HTTPException(status_code=422, detail="validation_error") from error
    return {
        "spec": parsed.spec.model_dump(
            mode="json", by_alias=True, exclude_none=False, exclude_unset=True
        ),
        "mode": parsed.mode,
        "output": {
            "width": parsed.output.width,
            "height": parsed.output.height,
        },
    }


async def _trusted_context(
    owner_id: UUID, client_context: dict, *, force_patch: bool
) -> dict:
    """Replace browser-asserted authorization and Compute capability fields."""

    # Rebuild rather than mutate the browser dictionary. Even a direct caller
    # cannot carry an unrecognised prompt/authorization field past this trust
    # boundary; membership and Compute data are always server projections.
    context = {
        "spec": client_context["spec"],
        "mode": client_context["mode"],
        "output": client_context["output"],
    }
    async with get_engine().connect() as connection:
        context["member"] = bool(await connection.scalar(sql(
            "SELECT 1 FROM memberships WHERE user_id=:owner AND status='active'"
        ), {"owner": owner_id}))
    try:
        context["capabilities"] = await studio_capabilities()
    except ComputeClientError as error:
        context["capabilities"] = {}
        if force_patch:
            raise HTTPException(
                status_code=503,
                detail="COMPUTE_CAPACITY_EXHAUSTED",
            ) from error
    return context


async def _read_image(upload: UploadFile | None) -> tuple[bytes | None, str | None]:
    if upload is None:
        return None, None
    if upload.content_type not in {"image/png", "image/jpeg", "image/webp"}:
        raise HTTPException(status_code=422, detail="ai_image_invalid")
    limit = get_settings().ai_max_image_bytes
    data = await upload.read(limit + 1)
    if len(data) > limit:
        raise HTTPException(status_code=413, detail="payload_too_large")
    try:
        with Image.open(BytesIO(data)) as image:
            image.verify()
        with Image.open(BytesIO(data)) as image:
            if max(image.size) > 640:
                raise HTTPException(status_code=422, detail="ai_image_dimensions_invalid")
    except (UnidentifiedImageError, OSError) as error:
        raise HTTPException(status_code=422, detail="ai_image_invalid") from error
    try:
        return prepare_ai_image(data, upload.content_type)
    except ValueError as error:
        raise HTTPException(status_code=422, detail="ai_image_invalid") from error


@router.post("/ai/conversations/{conversation_id}/messages")
async def post_message(conversation_id: UUID, request: Request,
                       text: str = Form(...), context: str = Form(default="{}"),
                       request_patch: bool = Form(default=False, alias="requestPatch"),
                       assistant_mode: AssistantMode = Form(default="chat", alias="assistantMode"),
                       image: UploadFile | None = File(default=None),
                       idempotency_key: str = Header(..., alias="Idempotency-Key"),
                       principal: AccessPrincipal = Depends(require_principal)) -> Response:
    enforce_origin_and_csrf(request, principal)
    settings = get_settings()
    if not settings.ai_enabled:
        raise HTTPException(status_code=503, detail="AI_DISABLED")
    user_text = text.strip()
    if not user_text or len(user_text) > settings.ai_max_user_message_chars:
        raise HTTPException(status_code=422, detail="validation_error")
    # Direct unit calls do not run FastAPI's dependency coercion and therefore
    # see the Form descriptor itself. HTTP requests are still constrained by
    # the AssistantMode Literal at the boundary.
    resolved_mode: AssistantMode = (
        assistant_mode
        if assistant_mode in {"chat", "location", "color", "composition"}
        else "chat"
    )
    requires_suggestion = request_patch or resolved_mode != "chat"
    parsed_context = await _trusted_context(
        principal.user_id,
        _parse_context(context),
        force_patch=requires_suggestion,
    )
    image_data, image_type = await _read_image(image)
    if resolved_mode != "chat" and image_data is None:
        raise HTTPException(status_code=422, detail="ai_image_required")
    iterator = stream_message(owner_id=principal.user_id, conversation_id=conversation_id,
        idempotency_key=idempotency_key, user_text=user_text, context=parsed_context,
        image=image_data, image_type=image_type, force_patch=request_patch,
        assistant_mode=resolved_mode)
    try:
        first = await anext(iterator)
    except StopAsyncIteration:
        raise HTTPException(status_code=503, detail="AI_PROVIDER_UNAVAILABLE") from None

    async def body() -> AsyncIterator[bytes]:
        try:
            yield first
            async for chunk in iterator:
                yield chunk
        finally:
            await iterator.aclose()
    return StreamingResponse(body(), media_type="text/event-stream",
        headers={"Cache-Control": "no-store, no-transform", "X-Accel-Buffering": "no"})


@router.post("/ai/messages/{message_id}/feedback")
async def feedback(message_id: UUID, payload: FeedbackInput, request: Request,
                   idempotency_key: str = Header(..., alias="Idempotency-Key"),
                   principal: AccessPrincipal = Depends(require_principal)) -> Response:
    enforce_origin_and_csrf(request, principal)
    async with get_engine().begin() as c:
        await lock_ai_owner(c, principal.user_id)
        row = (await c.execute(sql(
            "SELECT c.id AS conversation_id,r.status FROM ai_messages m "
            "JOIN ai_conversations c ON c.id=m.conversation_id "
            "JOIN ai_requests r ON r.assistant_message_id=m.id "
            "WHERE m.id=:id AND m.role='assistant' AND c.owner_id=:owner "
            "FOR UPDATE OF c,m,r"
        ), {"id": message_id, "owner": principal.user_id})).mappings().one_or_none()
        if row is None:
            raise HTTPException(status_code=404, detail="ai_message_not_found")
        if row["status"] != "completed":
            raise HTTPException(status_code=409, detail="ai_message_not_complete")
        conversation_id = row["conversation_id"]
        body_input = payload.model_dump()
        claim = await idempotency_service.claim(c, user_id=principal.user_id,
            scope=f"ai.feedback:{message_id}", key=idempotency_key, body=body_input)
        if response := _replay(claim):
            return response
        await c.execute(sql(
            "INSERT INTO ai_feedback(id,message_id,owner_id,rating,consent) VALUES(:id,:mid,:owner,:rating,:consent) "
            "ON CONFLICT(message_id) DO UPDATE SET rating=excluded.rating,consent=excluded.consent,created_at=now()"
        ), {"id": uuid4(), "mid": message_id, "owner": principal.user_id,
            "rating": payload.rating, "consent": payload.consent})
        await recompute_conversation_optimization_consent(c, conversation_id)
        body = {"data": {"messageId": str(message_id), "rating": payload.rating,
                         "consent": payload.consent}}
        await idempotency_service.complete(c, claim, response_status=200, response_body=body)
    return JSONResponse(content=body)


@router.get("/me/ai-allowance")
async def get_allowance(principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    result = await allowance(principal.user_id)
    result["enabled"] = get_settings().ai_enabled
    return {"data": result}
