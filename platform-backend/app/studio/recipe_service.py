"""Canonical immutable fractal structure lifecycle."""

from __future__ import annotations

import base64
import hashlib
import json
from dataclasses import dataclass
from datetime import datetime
from typing import Any
from uuid import UUID

import orjson
from fastapi import HTTPException, Request, status

from app.core import audit_writer, idempotency_service
from app.core.db import get_engine
from app.core.request_context import request_id
from app.studio import recipe_repository
from app.studio.models import CursorPage, FractalSpec, RecipeCollectionView, RecipeView


RENDERER_VERSION = "map-v1"


@dataclass(frozen=True, slots=True)
class CanonicalRecipe:
    spec: dict[str, object]
    sha256: str
    structure_version: int


def canonicalize_spec(spec: FractalSpec) -> CanonicalRecipe:
    """Produce byte-stable JSON before hashing; never hash caller key order or whitespace."""
    normalized = spec.model_dump(mode="json", by_alias=True, exclude_none=True)
    encoded = orjson.dumps(normalized, option=orjson.OPT_SORT_KEYS)
    return CanonicalRecipe(
        spec=orjson.loads(encoded),
        sha256=hashlib.sha256(encoded).hexdigest(),
        structure_version=spec.version,
    )


def _view(record: recipe_repository.RecipeRecord) -> RecipeView:
    return RecipeView(
        id=record.id,
        owner_id=record.owner_id,
        canonical_spec=record.canonical_spec,
        spec_hash=record.spec_hash,
        renderer_version=record.renderer_version,
        created_at=record.created_at,
    )


def _encode_cursor(record: recipe_repository.RecipeRecord) -> str:
    raw = json.dumps(
        {"createdAt": record.created_at.isoformat(), "id": str(record.id)}, separators=(",", ":")
    ).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _decode_cursor(cursor: str | None) -> tuple[datetime | None, UUID | None]:
    if cursor is None:
        return None, None
    try:
        padded = cursor + "=" * (-len(cursor) % 4)
        value: dict[str, Any] = json.loads(base64.urlsafe_b64decode(padded))
        return datetime.fromisoformat(value["createdAt"]), UUID(value["id"])
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_cursor") from error


async def create_or_reuse(
    *,
    owner_id: UUID,
    canonical: CanonicalRecipe,
    idempotency_key: str,
    request: Request,
) -> tuple[dict[str, object], int, bool, dict[str, str]]:
    """Recipe insert, audit and idempotency completion share one PostgreSQL transaction."""
    async with get_engine().begin() as connection:
        claim = await idempotency_service.claim(
            connection,
            user_id=owner_id,
            scope="studio.recipe.create",
            key=idempotency_key,
            body={"canonicalSpec": canonical.spec},
        )
        if claim.is_replay:
            return (
                claim.replay_body or {},
                claim.replay_status or status.HTTP_200_OK,
                True,
                claim.replay_headers or {},
            )
        record, created = await recipe_repository.insert_or_find(
            connection,
            owner_id=owner_id,
            canonical_spec=canonical.spec,
            spec_hash=canonical.sha256,
            structure_version=canonical.structure_version,
            renderer_version=RENDERER_VERSION,
        )
        if created:
            await audit_writer.record_user_action(
                connection,
                actor_user_id=owner_id,
                action="recipe.created",
                subject_type="fractal_recipe",
                subject_id=record.id,
                request_id_value=request_id(request),
                metadata={"specHash": canonical.sha256},
            )
        body: dict[str, object] = {"data": _view(record).model_dump(mode="json", by_alias=True)}
        response_status = status.HTTP_201_CREATED if created else status.HTTP_200_OK
        response_headers = {"Cache-Control": "no-store"}
        await idempotency_service.complete(
            connection,
            claim,
            response_status=response_status,
            response_body=body,
            response_headers=response_headers,
        )
    return body, response_status, False, response_headers


async def list_recipes(*, owner_id: UUID, cursor: str | None, limit: int) -> RecipeCollectionView:
    before_created_at, before_id = _decode_cursor(cursor)
    async with get_engine().connect() as connection:
        records = await recipe_repository.list_owned(
            connection,
            owner_id=owner_id,
            before_created_at=before_created_at,
            before_id=before_id,
            limit=limit + 1,
        )
    has_next = len(records) > limit
    page_records = records[:limit]
    next_cursor = _encode_cursor(page_records[-1]) if has_next and page_records else None
    return RecipeCollectionView(data=[_view(record) for record in page_records], page=CursorPage(next_cursor=next_cursor))
