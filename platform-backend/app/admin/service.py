"""Administrator application service with audit and safety invariants."""

from __future__ import annotations

import asyncio
from datetime import datetime
from decimal import Decimal
from uuid import UUID

import httpx
from fastapi import HTTPException, Request, status

from app.admin import repository
from app.admin.models import (
    AdminListingModerationInput,
    AdminListingView,
    AdminComputeNodeView,
    AdminStatisticsView,
    AdminUserUpdateInput,
    AdminUserView,
    CommerceStatistics,
    ComputeStatistics,
    MarketStatistics,
    UserStatistics,
)
from app.assets.ports import AssetReader
from app.assets.reader import AssetReadService
from app.auth.models import AccessPrincipal
from app.core import audit_writer, idempotency_service
from app.core.config import get_settings
from app.core.db import get_engine
from app.core.request_context import request_id
from app.marketplace.models import PreviewView


def _money(value: object) -> str:
    return format(Decimal(value or 0), ".2f")


def _user_view(record: repository.AdminUserRecord) -> AdminUserView:
    profile = None
    if record.creator_handle is not None:
        profile = {
            "handle": record.creator_handle,
            "displayName": record.creator_display_name or record.creator_handle,
        }
    return AdminUserView(
        id=record.id,
        email=record.email,
        status=record.status,
        roles=record.roles,
        member=record.member,
        creatorProfile=profile,
        assetCount=record.asset_count,
        listingCount=record.listing_count,
        fulfilledOrderCount=record.fulfilled_order_count,
        createdAt=record.created_at,
    )


def _ensure_admin_creator_separation(
    record: repository.AdminUserRecord, privileged_roles: set[str]
) -> None:
    if "admin" in privileged_roles and (
        "creator" in record.roles or record.creator_handle is not None
    ):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="admin_creator_role_conflict",
        )


class AdminService:
    def __init__(self, *, assets: AssetReader | None = None) -> None:
        self._assets = assets or AssetReadService()

    async def statistics(self) -> AdminStatisticsView:
        async with get_engine().connect() as connection:
            values = await repository.statistics(connection)
        return AdminStatisticsView(
            generatedAt=values["generated_at"],
            users=UserStatistics(
                total=values["users_total"],
                active=values["users_active"],
                disabled=values["users_disabled"],
                creators=values["creators"],
                members=values["members"],
                admins=values["admins"],
                newLast30Days=values["users_new_30d"],
            ),
            market=MarketStatistics(
                listings=values["listings_total"],
                published=values["listings_published"],
                draft=values["listings_draft"],
                unpublished=values["listings_unpublished"],
                archived=values["listings_archived"],
                readyAssets=values["ready_assets"],
                favorites=values["favorites"],
            ),
            commerce=CommerceStatistics(
                orders=values["orders_total"],
                fulfilled=values["orders_fulfilled"],
                pendingPayment=values["orders_pending"],
                paymentExceptions=values["orders_exception"],
                marketplaceGrossCny=_money(values["marketplace_gross"]),
                membershipRevenueCny=_money(values["membership_revenue"]),
                creatorRevenueCny=_money(values["creator_revenue"]),
                platformRevenueCny=_money(values["platform_revenue"]),
            ),
            compute=ComputeStatistics(
                renderJobs=values["render_jobs_total"],
                active=values["render_jobs_active"],
                completed=values["render_jobs_completed"],
                failed=values["render_jobs_failed"],
            ),
        )

    async def compute_nodes(self) -> list[AdminComputeNodeView]:
        settings = get_settings()
        if not settings.compute_gateway_admin_key:
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="compute_admin_unavailable",
            )
        try:
            async with httpx.AsyncClient(
                base_url=settings.compute_base_url,
                headers={"Authorization": f"Bearer {settings.compute_gateway_admin_key}"},
                timeout=httpx.Timeout(10.0, connect=settings.compute_connect_timeout_seconds),
                trust_env=False,
            ) as client:
                response = await client.get("/internal/v1/nodes")
                response.raise_for_status()
                payload = response.json()
                if not isinstance(payload, dict) or not isinstance(payload.get("data"), list):
                    raise ValueError("invalid compute node response")
                return [AdminComputeNodeView.model_validate(row) for row in payload["data"]]
        except (httpx.HTTPError, ValueError, TypeError) as error:
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="compute_admin_unavailable",
            ) from error

    async def list_users(
        self,
        *,
        q: str | None,
        user_status: str | None,
        role: str | None,
        before: tuple[datetime, UUID] | None,
        limit: int,
    ) -> tuple[list[AdminUserView], list[repository.AdminUserRecord]]:
        async with get_engine().connect() as connection:
            records = await repository.list_users(
                connection,
                q=q,
                user_status=user_status,
                role=role,
                before=before,
                limit=limit,
            )
        return [_user_view(record) for record in records], records

    async def update_user(
        self,
        *,
        principal: AccessPrincipal,
        user_id: UUID,
        payload: AdminUserUpdateInput,
        idempotency_key: str,
        request: Request,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection,
                user_id=principal.user_id,
                scope=f"admin.update_user:{user_id}",
                key=idempotency_key,
                body=payload.model_dump(mode="json", by_alias=True, exclude_none=True),
            )
            if claim.is_replay:
                return (
                    claim.replay_body or {},
                    claim.replay_status or 200,
                    claim.replay_headers or {},
                )
            locked = await repository.lock_user(connection, user_id)
            current = await repository.find_user(connection, user_id)
            if locked is None or current is None:
                raise HTTPException(
                    status_code=status.HTTP_404_NOT_FOUND, detail="admin_user_not_found"
                )

            next_status = payload.status or current.status
            next_member = current.member if payload.member is None else payload.member
            current_privileged = {
                role for role in current.roles if role in {"admin", "finance_operator"}
            }
            next_privileged = (
                current_privileged
                if payload.privileged_roles is None
                else set(payload.privileged_roles)
            )
            _ensure_admin_creator_separation(current, next_privileged)
            if user_id == principal.user_id and next_status != "active":
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT, detail="cannot_disable_self"
                )
            if user_id == principal.user_id and "admin" not in next_privileged:
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT, detail="cannot_remove_own_admin"
                )
            if "admin" in current_privileged and "admin" not in next_privileged:
                if await repository.admin_count(connection) <= 1:
                    raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="last_admin")

            await repository.update_user(
                connection,
                user_id=user_id,
                user_status=next_status,
                member=next_member,
                privileged_roles=next_privileged,
                membership_granted_by=principal.user_id,
            )
            updated = await repository.find_user(connection, user_id)
            assert updated is not None
            view = _user_view(updated)
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection,
                actor_user_id=principal.user_id,
                action="admin.user_updated",
                subject_type="user",
                subject_id=user_id,
                request_id_value=request_id(request),
                metadata={
                    "statusBefore": current.status,
                    "statusAfter": updated.status,
                    "memberBefore": current.member,
                    "memberAfter": updated.member,
                    "privilegedRolesBefore": sorted(current_privileged),
                    "privilegedRolesAfter": sorted(next_privileged),
                },
            )
            await idempotency_service.complete(
                connection,
                claim,
                response_status=200,
                response_body=body,
                response_headers=headers,
            )
        return body, 200, headers

    async def list_listings(
        self,
        *,
        q: str | None,
        listing_status: str | None,
        before: tuple[datetime, UUID] | None,
        limit: int,
    ) -> tuple[list[AdminListingView], list[repository.AdminListingRecord]]:
        async with get_engine().connect() as connection:
            records = await repository.list_listings(
                connection,
                q=q,
                listing_status=listing_status,
                before=before,
                limit=limit,
            )
        views = list(await asyncio.gather(*(self._listing_view(record) for record in records)))
        return views, records

    async def moderate_listing(
        self,
        *,
        principal: AccessPrincipal,
        listing_id: UUID,
        payload: AdminListingModerationInput,
        idempotency_key: str,
        request: Request,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection,
                user_id=principal.user_id,
                scope=f"admin.moderate_listing:{listing_id}",
                key=idempotency_key,
                body=payload.model_dump(mode="json"),
            )
            if claim.is_replay:
                return (
                    claim.replay_body or {},
                    claim.replay_status or 200,
                    claim.replay_headers or {},
                )
            locked = await repository.lock_listing(connection, listing_id)
            if locked is None:
                raise HTTPException(
                    status_code=status.HTTP_404_NOT_FOUND, detail="listing_not_found"
                )
            previous_status = str(locked["status"])
            if payload.action == "unpublish" and previous_status != "published":
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT, detail="listing_not_unpublishable"
                )
            if payload.action == "archive" and previous_status == "archived":
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT, detail="listing_already_archived"
                )
            await repository.moderate_listing(
                connection, listing_id=listing_id, action=payload.action
            )
            updated = await repository.find_listing(connection, listing_id)
            assert updated is not None
            view = await self._listing_view(updated)
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection,
                actor_user_id=principal.user_id,
                action=(
                    "admin.listing_unpublished"
                    if payload.action == "unpublish"
                    else "admin.listing_archived"
                ),
                subject_type="listing",
                subject_id=listing_id,
                request_id_value=request_id(request),
                metadata={"reason": payload.reason, "statusBefore": previous_status},
            )
            await idempotency_service.complete(
                connection,
                claim,
                response_status=200,
                response_body=body,
                response_headers=headers,
            )
        return body, 200, headers

    async def _listing_view(self, record: repository.AdminListingRecord) -> AdminListingView:
        preview = await self._assets.find_public_preview(asset_id=record.asset_id)
        preview_view = None
        if preview is not None:
            preview_view = PreviewView(
                mediaType=preview.media_type,
                thumbnailUrl=preview.thumbnail_url,
                watermarkedPreviewUrl=preview.watermarked_preview_url,
                videoPosterUrl=preview.video_poster_url,
            )
        return AdminListingView(
            id=record.id,
            assetId=record.asset_id,
            creatorId=record.creator_id,
            creatorEmail=record.creator_email,
            creatorHandle=record.creator_handle,
            creatorDisplayName=record.creator_display_name,
            status=record.status,
            title=record.title,
            description=record.description,
            tags=record.tags,
            price=format(record.price, ".2f"),
            currency="CNY",
            mediaType=record.media_type,
            favoriteCount=record.favorite_count,
            saleCount=record.sale_count,
            createdAt=record.created_at,
            publishedAt=record.published_at,
            preview=preview_view,
        )
