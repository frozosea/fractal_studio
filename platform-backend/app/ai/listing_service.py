"""Owner-scoped listing copy generation with the shared lifetime AI ledger."""

from __future__ import annotations

import asyncio
import hashlib
import io
import json
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from uuid import UUID, uuid4

from fastapi import HTTPException, status
from PIL import Image, ImageOps, UnidentifiedImageError
from pydantic import ValidationError
from sqlalchemy import text as sql

from app.ai.listing_models import (
    ListingCopyCandidate,
    ListingCopyInput,
    ListingCopyToolResult,
    listing_candidates_json,
)
from app.ai.listing_provider import ListingProviderUnavailable, generate_listing_copy
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.infrastructure.storage.object_storage import ObjectStorage


_TRIAL_STATUSES = "'reserved','streaming','retrying','partial','completed'"
_ACTIVE_STATUSES = "'reserved','streaming','retrying'"
_REQUEST_LEASE_SECONDS = 240
_MAX_PREVIEW_SOURCE_BYTES = 16 * 1024 * 1024
_MAX_PREVIEW_SOURCE_PIXELS = 16_777_216


class ListingPreviewUnavailable(Exception):
    """The owned listing does not currently have a safe image for visual analysis."""


@dataclass(frozen=True, slots=True)
class ListingSource:
    listing_id: UUID
    asset_id: UUID
    status: str
    title: str
    description: str
    tags: list[str]
    canonical_spec: dict[str, object]
    output_spec: dict[str, object]
    preview_object_key: str


@dataclass(frozen=True, slots=True)
class ListingRevision:
    request_id: UUID
    candidates: list[dict[str, object]]


@dataclass(frozen=True, slots=True)
class ListingReservation:
    request_id: UUID
    replay: list[ListingCopyCandidate] | None
    attempt_started_at: datetime | None


def _safe_spec_context(source: ListingSource) -> dict[str, object]:
    """Keep grounding fields while withholding coordinates and custom formula source."""

    spec = source.canonical_spec
    fields = (
        "variant",
        "iterations",
        "metric",
        "colorMap",
        "colorMode",
        "cyclesPerOctave",
        "rotationDeg",
        "julia",
        "transitionMode",
    )
    result = {field: spec[field] for field in fields if field in spec}
    if "orbitProgram" in spec:
        result["formulaKind"] = "custom"
        result.pop("variant", None)
    if isinstance(spec.get("colorProgram"), dict):
        stops = spec["colorProgram"].get("stops")
        result["colorProgram"] = {
            "kind": "customGradient",
            "stopCount": len(stops) if isinstance(stops, list) else None,
        }
        result.pop("colorMap", None)
    result["output"] = {
        key: source.output_spec[key]
        for key in ("width", "height", "resolution", "mediaType")
        if key in source.output_spec
    }
    return result


def _listing_context(source: ListingSource) -> dict[str, object]:
    return {
        "existingTitle": source.title,
        "existingDescription": source.description,
        "existingTags": source.tags,
        "render": _safe_spec_context(source),
    }


def prepare_listing_preview(
    source_image: bytes,
    *,
    max_bytes: int = 1_048_576,
) -> tuple[bytes, str]:
    """Decode and resize an asset derivative entirely in memory."""

    target_bytes = min(max_bytes, 1_048_576)
    if target_bytes < 1024:
        raise ListingPreviewUnavailable("configured image limit is too small")
    try:
        with Image.open(io.BytesIO(source_image)) as opened:
            if opened.width * opened.height > _MAX_PREVIEW_SOURCE_PIXELS:
                raise ListingPreviewUnavailable("asset preview dimensions are too large")
            opened.load()
            image = ImageOps.exif_transpose(opened).convert("RGB")
    except (UnidentifiedImageError, OSError, ValueError, Image.DecompressionBombError) as error:
        raise ListingPreviewUnavailable("asset preview is not a valid image") from error
    if image.width < 1 or image.height < 1:
        raise ListingPreviewUnavailable("asset preview has invalid dimensions")
    image.thumbnail((640, 640), Image.Resampling.LANCZOS)
    # JPEG is predictable for the hard request-size limit and is fully
    # sufficient for listing-copy visual analysis. Quality is lowered only as
    # much as required; dimensions remain useful for visual grounding.
    for quality in (94, 90, 86, 80, 72, 64, 54, 44):
        output = io.BytesIO()
        image.save(output, format="JPEG", quality=quality, optimize=True)
        data = output.getvalue()
        if len(data) <= target_bytes:
            return data, "image/jpeg"
    raise ListingPreviewUnavailable("asset preview cannot fit the AI image limit")


async def load_listing_source(
    *,
    owner_id: UUID,
    listing_id: UUID,
    source_request_id: UUID | None,
    locale: str,
) -> tuple[ListingSource, ListingRevision | None]:
    async with get_engine().connect() as connection:
        row = (
            await connection.execute(
                sql(
                    """
                    SELECT l.id,l.asset_id,l.status::text AS status,l.title,l.description,
                           a.status::text AS asset_status,r.canonical_spec,j.output_spec_json,
                           COALESCE((SELECT array_agg(lt.tag ORDER BY lt.tag)
                                     FROM listing_tags lt WHERE lt.listing_id=l.id),
                                    ARRAY[]::text[]) AS tags,
                           preview.object_key AS preview_object_key
                    FROM listings l
                    JOIN assets a ON a.id=l.asset_id
                    JOIN fractal_recipes r ON r.id=a.recipe_id
                    JOIN render_jobs j ON j.id=a.render_job_id
                    LEFT JOIN LATERAL (
                      SELECT f.object_key
                      FROM asset_files f
                      WHERE f.asset_id=a.id
                        AND f.purpose IN ('thumbnail','watermarked_preview')
                      ORDER BY CASE f.purpose::text WHEN 'thumbnail' THEN 0 ELSE 1 END
                      LIMIT 1
                    ) preview ON true
                    WHERE l.id=:listing_id AND l.creator_id=:owner_id
                    """
                ),
                {"listing_id": listing_id, "owner_id": owner_id},
            )
        ).mappings().one_or_none()
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
        if str(row["status"]) not in {"draft", "unpublished"}:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="invalid_state")
        if str(row["asset_status"]) != "ready" or not row["preview_object_key"]:
            raise ListingPreviewUnavailable("listing asset preview is not ready")
        source = ListingSource(
            listing_id=row["id"],
            asset_id=row["asset_id"],
            status=str(row["status"]),
            title=str(row["title"]),
            description=str(row["description"]),
            tags=[str(tag) for tag in row["tags"]],
            canonical_spec=dict(row["canonical_spec"]),
            output_spec=dict(row["output_spec_json"]),
            preview_object_key=str(row["preview_object_key"]),
        )
        revision = None
        if source_request_id is not None:
            prior = (
                await connection.execute(
                    sql(
                        """
                        SELECT request_id,candidates
                        FROM ai_listing_copy_results
                        WHERE request_id=:request_id AND owner_id=:owner_id
                          AND listing_id=:listing_id AND locale=:locale
                          AND expires_at>now()
                        """
                    ),
                    {
                        "request_id": source_request_id,
                        "owner_id": owner_id,
                        "listing_id": listing_id,
                        "locale": locale,
                    },
                )
            ).mappings().one_or_none()
            if prior is None:
                # Owner/listing/age are deliberately indistinguishable.
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
            try:
                checked = ListingCopyToolResult.model_validate(
                    {"candidates": prior["candidates"]}
                )
            except ValidationError as error:
                raise HTTPException(status_code=409, detail="invalid_state") from error
            revision = ListingRevision(
                request_id=prior["request_id"],
                candidates=listing_candidates_json(checked.candidates),
            )
    return source, revision


def _request_hash(
    *,
    source: ListingSource,
    locale: str,
    revision: ListingRevision | None,
    instruction: str | None,
) -> str:
    fingerprint = {
        "listingId": str(source.listing_id),
        "assetId": str(source.asset_id),
        "locale": locale,
        "title": source.title,
        "description": source.description,
        "tags": source.tags,
        "canonicalSpec": source.canonical_spec,
        "outputSpec": source.output_spec,
        "previewObject": source.preview_object_key,
        "sourceRequestId": str(revision.request_id) if revision else None,
        "instruction": instruction,
    }
    encoded = json.dumps(
        fingerprint,
        sort_keys=True,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode()
    return hashlib.sha256(encoded).hexdigest()


async def reserve_listing_copy(
    *,
    owner_id: UUID,
    idempotency_key: str,
    request_hash: str,
) -> ListingReservation:
    settings = get_settings()
    if not 1 <= len(idempotency_key) <= 200:
        raise HTTPException(status_code=422, detail="invalid_idempotency_key")
    async with get_engine().begin() as connection:
        lock_key = int.from_bytes(owner_id.bytes[:8], "big", signed=True)
        await connection.execute(
            sql("SELECT pg_advisory_xact_lock(:lock_key)"),
            {"lock_key": lock_key},
        )
        await connection.execute(
            sql(
                "UPDATE ai_requests SET "
                "status=CASE WHEN first_output_at IS NULL THEN 'released' ELSE 'partial' END,"
                "completed_at=now(),lease_until=NULL "
                f"WHERE owner_id=:owner_id AND status IN ({_ACTIVE_STATUSES}) "
                "AND (lease_until IS NULL OR lease_until<=now())"
            ),
            {"owner_id": owner_id},
        )
        prior = (
            await connection.execute(
                sql(
                    """
                    SELECT r.id AS request_id,r.status,r.request_hash,
                           result.candidates,result.expires_at
                    FROM ai_requests r
                    LEFT JOIN ai_listing_copy_results result
                      ON result.request_id=r.id AND result.expires_at>now()
                    WHERE r.owner_id=:owner_id AND r.idempotency_key=:idempotency_key
                    """
                ),
                {"owner_id": owner_id, "idempotency_key": idempotency_key},
            )
        ).mappings().one_or_none()
        if prior:
            if prior["request_hash"] != request_hash:
                raise HTTPException(status_code=409, detail="idempotency_conflict")
            if prior["status"] == "completed":
                if prior["candidates"] is None or prior["expires_at"] is None:
                    raise HTTPException(status_code=409, detail="idempotency_conflict")
                try:
                    replay = ListingCopyToolResult.model_validate(
                        {"candidates": prior["candidates"]}
                    ).candidates
                except ValidationError as error:
                    raise HTTPException(status_code=409, detail="idempotency_conflict") from error
                return ListingReservation(
                    request_id=prior["request_id"],
                    replay=replay,
                    attempt_started_at=None,
                )
            if prior["status"] not in {"released", "failed"}:
                raise HTTPException(status_code=409, detail="idempotency_in_progress")
        member = bool(
            await connection.scalar(
                sql("SELECT 1 FROM memberships WHERE user_id=:owner_id AND status='active'"),
                {"owner_id": owner_id},
            )
        )
        counts = (
            await connection.execute(
                sql(
                    "SELECT "
                    f"count(*) FILTER (WHERE status IN ({_ACTIVE_STATUSES})) AS active,"
                    "count(*) FILTER (WHERE counts_toward_trial "
                    f"AND status IN ({_TRIAL_STATUSES})) AS trial_used "
                    "FROM ai_requests WHERE owner_id=:owner_id"
                ),
                {"owner_id": owner_id},
            )
        ).mappings().one()
        active = int(counts["active"] or 0)
        trial_used = int(counts["trial_used"] or 0)
        if active >= settings.ai_max_concurrent_per_user:
            raise HTTPException(status_code=429, detail="ai_concurrency_exhausted")
        counts_toward_trial = not member
        if counts_toward_trial and trial_used >= settings.ai_free_lifetime_limit:
            raise HTTPException(status_code=402, detail="AI_TRIAL_EXHAUSTED")
        attempt_started_at = datetime.now(timezone.utc)
        lease_until = attempt_started_at + timedelta(seconds=_REQUEST_LEASE_SECONDS)
        if prior:
            request_id = prior["request_id"]
            updated = await connection.execute(
                sql(
                    "UPDATE ai_requests SET status='reserved',first_output_at=NULL,"
                    "completed_at=NULL,attempt_started_at=:attempt_started_at,"
                    "lease_until=:lease_until,counts_toward_trial=:counts_toward_trial "
                    "WHERE id=:request_id AND owner_id=:owner_id "
                    "AND status IN ('released','failed') RETURNING id"
                ),
                {
                    "request_id": request_id,
                    "owner_id": owner_id,
                    "attempt_started_at": attempt_started_at,
                    "lease_until": lease_until,
                    "counts_toward_trial": counts_toward_trial,
                },
            )
            if updated.scalar_one_or_none() is None:
                raise HTTPException(status_code=409, detail="idempotency_in_progress")
        else:
            request_id = uuid4()
            await connection.execute(
                sql(
                    """
                    INSERT INTO ai_requests(
                      id,owner_id,conversation_id,user_message_id,idempotency_key,status,request_hash,
                      counts_toward_trial,attempt_started_at,lease_until
                    ) VALUES(
                      :request_id,:owner_id,NULL,NULL,:idempotency_key,'reserved',:request_hash,
                      :counts_toward_trial,:attempt_started_at,:lease_until
                    )
                    """
                ),
                {
                    "request_id": request_id,
                    "owner_id": owner_id,
                    "idempotency_key": idempotency_key,
                    "request_hash": request_hash,
                    "counts_toward_trial": counts_toward_trial,
                    "attempt_started_at": attempt_started_at,
                    "lease_until": lease_until,
                },
            )
    return ListingReservation(
        request_id=request_id,
        replay=None,
        attempt_started_at=attempt_started_at,
    )


async def _release(request_id: UUID, attempt_started_at: datetime) -> None:
    async with get_engine().begin() as connection:
        await connection.execute(
            sql(
                "UPDATE ai_requests SET status='released',completed_at=now(),lease_until=NULL "
                "WHERE id=:request_id AND attempt_started_at=:attempt_started_at "
                "AND status='reserved'"
            ),
            {"request_id": request_id, "attempt_started_at": attempt_started_at},
        )


async def _refresh_lease(request_id: UUID, attempt_started_at: datetime) -> None:
    """Keep both paid provider phases inside one owned, non-billable attempt."""

    async with get_engine().begin() as connection:
        refreshed = await connection.execute(
            sql(
                "UPDATE ai_requests SET "
                "lease_until=now()+make_interval(secs=>:lease_seconds) "
                "WHERE id=:request_id AND attempt_started_at=:attempt_started_at "
                "AND status='reserved' RETURNING id"
            ),
            {
                "request_id": request_id,
                "attempt_started_at": attempt_started_at,
                "lease_seconds": _REQUEST_LEASE_SECONDS,
            },
        )
        if refreshed.scalar_one_or_none() is None:
            raise ListingProviderUnavailable("listing-copy reservation lease was lost")


async def _complete(
    *,
    request_id: UUID,
    owner_id: UUID,
    source: ListingSource,
    locale: str,
    revision: ListingRevision | None,
    candidates: list[ListingCopyCandidate],
    attempt_started_at: datetime,
) -> None:
    settings = get_settings()
    async with get_engine().begin() as connection:
        await connection.execute(
            sql(
                """
                INSERT INTO ai_listing_copy_results(
                  request_id,owner_id,listing_id,source_request_id,locale,candidates,expires_at
                ) VALUES(
                  :request_id,:owner_id,:listing_id,:source_request_id,:locale,
                  CAST(:candidates AS jsonb),now()+make_interval(days=>:history_days)
                )
                """
            ),
            {
                "request_id": request_id,
                "owner_id": owner_id,
                "listing_id": source.listing_id,
                "source_request_id": revision.request_id if revision else None,
                "locale": locale,
                "candidates": json.dumps(
                    listing_candidates_json(candidates),
                    ensure_ascii=False,
                    separators=(",", ":"),
                ),
                "history_days": settings.ai_history_ttl_days,
            },
        )
        updated = await connection.execute(
            sql(
                "UPDATE ai_requests SET status='completed',first_output_at=now(),"
                "completed_at=now(),lease_until=NULL "
                "WHERE id=:request_id AND owner_id=:owner_id "
                "AND attempt_started_at=:attempt_started_at AND status='reserved'"
            ),
            {
                "request_id": request_id,
                "owner_id": owner_id,
                "attempt_started_at": attempt_started_at,
            },
        )
        if updated.rowcount != 1:
            raise RuntimeError("listing_copy_reservation_lost")


async def listing_allowance(owner_id: UUID) -> dict[str, object]:
    settings = get_settings()
    async with get_engine().connect() as connection:
        member = bool(
            await connection.scalar(
                sql("SELECT 1 FROM memberships WHERE user_id=:owner_id AND status='active'"),
                {"owner_id": owner_id},
            )
        )
        used = int(
            await connection.scalar(
                sql(
                    "SELECT count(*) FROM ai_requests WHERE owner_id=:owner_id "
                    "AND counts_toward_trial "
                    f"AND status IN ({_TRIAL_STATUSES})"
                ),
                {"owner_id": owner_id},
            )
            or 0
        )
    return {
        "member": member,
        "limit": None if member else settings.ai_free_lifetime_limit,
        "used": used,
        "remaining": None if member else max(0, settings.ai_free_lifetime_limit - used),
        "enabled": settings.ai_enabled,
    }


async def create_listing_copy(
    *,
    owner_id: UUID,
    payload: ListingCopyInput,
    idempotency_key: str,
    storage: ObjectStorage | None = None,
    settings: Settings | None = None,
) -> dict[str, object]:
    resolved = settings or get_settings()
    source, revision = await load_listing_source(
        owner_id=owner_id,
        listing_id=payload.listing_id,
        source_request_id=payload.source_request_id,
        locale=payload.locale,
    )
    fingerprint = _request_hash(
        source=source,
        locale=payload.locale,
        revision=revision,
        instruction=payload.instruction,
    )
    reservation = await reserve_listing_copy(
        owner_id=owner_id,
        idempotency_key=idempotency_key,
        request_hash=fingerprint,
    )
    if reservation.replay is not None:
        return {
            "requestId": str(reservation.request_id),
            "candidates": listing_candidates_json(reservation.replay),
            "allowance": await listing_allowance(owner_id),
        }
    request_id = reservation.request_id
    if reservation.attempt_started_at is None:
        raise RuntimeError("new listing-copy reservation has no attempt timestamp")
    attempt_started_at = reservation.attempt_started_at
    try:
        try:
            source_bytes = await (storage or ObjectStorage(resolved)).download_bytes(
                object_key=source.preview_object_key,
                max_bytes=_MAX_PREVIEW_SOURCE_BYTES,
            )
        except Exception as error:
            raise ListingPreviewUnavailable("listing preview read failed") from error
        image, image_type = prepare_listing_preview(
            source_bytes,
            max_bytes=resolved.ai_max_image_bytes,
        )
        completion = None
        for attempt in range(2):
            try:
                await _refresh_lease(request_id, attempt_started_at)
                completion = await generate_listing_copy(
                    locale=payload.locale,
                    listing_context=_listing_context(source),
                    image=image,
                    image_type=image_type,
                    prior_candidates=revision.candidates if revision else None,
                    instruction=payload.instruction,
                    settings=resolved,
                )
                break
            except ListingProviderUnavailable as error:
                if attempt == 1 or not error.retryable:
                    raise
        if completion is None:
            raise ListingProviderUnavailable("provider returned no completion")
        await asyncio.shield(
            _complete(
                request_id=request_id,
                owner_id=owner_id,
                source=source,
                locale=payload.locale,
                revision=revision,
                candidates=completion.candidates,
                attempt_started_at=attempt_started_at,
            )
        )
    except asyncio.CancelledError:
        await asyncio.shield(_release(request_id, attempt_started_at))
        raise
    except ListingProviderUnavailable:
        await _release(request_id, attempt_started_at)
        raise
    except (ListingPreviewUnavailable, ValueError):
        await _release(request_id, attempt_started_at)
        raise ListingPreviewUnavailable("listing preview unavailable") from None
    except Exception:
        await _release(request_id, attempt_started_at)
        raise
    return {
        "requestId": str(request_id),
        "candidates": listing_candidates_json(completion.candidates),
        "allowance": await listing_allowance(owner_id),
    }
