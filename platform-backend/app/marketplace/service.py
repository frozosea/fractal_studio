"""Listing lifecycle, catalogue and bookmarks application service."""

from __future__ import annotations

import asyncio
from datetime import datetime
from decimal import Decimal
from typing import Literal
from uuid import UUID

from fastapi import HTTPException, Request, status
from sqlalchemy.exc import IntegrityError

from app.assets.ports import AssetPreview, AssetReader
from app.assets.reader import AssetReadService
from app.auth.models import AccessPrincipal
from app.core import audit_writer, idempotency_service
from app.core.db import get_engine
from app.core.request_context import request_id
from app.marketplace import repository
from app.marketplace.licence_registry import LicenceRegistry
from app.marketplace.models import (
    CreatorView,
    FavoriteView,
    LicenceOfferInput,
    LicenceOfferView,
    ListingCreateInput,
    ListingUpdateInput,
    ListingView,
    PreviewView,
)
from app.marketplace.ports import PublishedOfferSnapshot


def _preview_view(preview: AssetPreview | None) -> PreviewView | None:
    if preview is None:
        return None
    return PreviewView(
        mediaType=preview.media_type,
        thumbnailUrl=preview.thumbnail_url,
        watermarkedPreviewUrl=preview.watermarked_preview_url,
        videoPosterUrl=preview.video_poster_url,
    )


async def _listing_view(record: repository.ListingRecord, assets: AssetReader) -> ListingView:
    preview = await assets.find_public_preview(asset_id=record.asset_id)
    return ListingView(
        id=record.id,
        assetId=record.asset_id,
        creator=CreatorView(id=record.creator_id, handle=record.creator_handle, displayName=record.creator_display_name),
        status=record.status,
        title=record.title,
        description=record.description,
        tags=record.tags,
        price=record.price,
        currency="CNY",
        publishedAt=record.published_at,
        preview=_preview_view(preview),
        licenceOffer=LicenceOfferView(
            id=record.licence_offer_id,
            code=record.licence_code,
            termsVersion=record.licence_terms_version,
            terms=record.licence_terms,
        ),
    )


class MarketplaceService:
    def __init__(self, *, assets: AssetReader | None = None, licences: LicenceRegistry | None = None) -> None:
        self._assets = assets or AssetReadService()
        self._licences = licences or LicenceRegistry()

    async def create_draft(
        self, *, principal: AccessPrincipal, payload: ListingCreateInput, idempotency_key: str, request: Request
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        owned = await self._assets.find_owned_asset(asset_id=payload.asset_id, owner_id=principal.user_id)
        if owned is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="asset_not_found")
        terms = self._licences.resolve_immutable_terms(
            code=payload.licence_offer.code, terms_version=payload.licence_offer.terms_version
        )
        try:
            async with get_engine().begin() as connection:
                claim = await idempotency_service.claim(
                    connection, user_id=principal.user_id, scope="marketplace.create_listing", key=idempotency_key,
                    body=payload.model_dump(mode="json", by_alias=True),
                )
                if claim.is_replay:
                    return claim.replay_body or {}, claim.replay_status or 201, claim.replay_headers or {}
                record = await repository.create_draft(
                    connection, asset_id=payload.asset_id, creator_id=principal.user_id, title=payload.title,
                    description=payload.description, tags=payload.tags, price=payload.price,
                    licence_code=payload.licence_offer.code, licence_terms_version=payload.licence_offer.terms_version,
                    licence_terms=terms,
                )
                view = await _listing_view(record, self._assets)
                body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
                headers = {"Cache-Control": "no-store"}
                await audit_writer.record_user_action(
                    connection, actor_user_id=principal.user_id, action="listing.draft_created",
                    subject_type="listing", subject_id=record.id, request_id_value=request_id(request),
                    metadata={"assetId": str(record.asset_id)},
                )
                await idempotency_service.complete(connection, claim, response_status=201, response_body=body, response_headers=headers)
        except IntegrityError as error:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="listing_asset_already_exists") from error
        return body, 201, headers

    async def update_draft(
        self, *, principal: AccessPrincipal, listing_id: UUID, payload: ListingUpdateInput,
        idempotency_key: str, request: Request,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        licence = self._resolve_update_licence(payload.licence_offer)
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope=f"marketplace.update_listing:{listing_id}", key=idempotency_key,
                body=payload.model_dump(mode="json", by_alias=True, exclude_none=True),
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 200, claim.replay_headers or {}
            current = await repository.lock_owned(connection, listing_id=listing_id, creator_id=principal.user_id)
            if current is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="listing_not_found")
            if current.status not in {"draft", "unpublished"}:
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="listing_not_editable")
            updated = await repository.update_draft(
                connection, listing=current, title=payload.title, description=payload.description, tags=payload.tags,
                price=payload.price, licence=licence,
            )
            view = await _listing_view(updated, self._assets)
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="listing.draft_updated", subject_type="listing",
                subject_id=listing_id, request_id_value=request_id(request),
            )
            await idempotency_service.complete(connection, claim, response_status=200, response_body=body, response_headers=headers)
        return body, 200, headers

    async def publish(
        self, *, principal: AccessPrincipal, listing_id: UUID, idempotency_key: str, request: Request
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope=f"marketplace.publish_listing:{listing_id}", key=idempotency_key,
                body={"listingId": str(listing_id)},
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 200, claim.replay_headers or {}
            current = await repository.lock_owned(connection, listing_id=listing_id, creator_id=principal.user_id)
            if current is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="listing_not_found")
            if current.status != "draft":
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="listing_not_publishable")
            publishable = await self._assets.find_publishable_asset(asset_id=current.asset_id, owner_id=principal.user_id)
            if publishable is None:
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="asset_not_publishable")
            snapshot = self._snapshot(current, media_type=publishable.asset.media_type)
            published = await repository.publish(connection, listing=current, snapshot=snapshot)
            view = await _listing_view(published, self._assets)
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="listing.published", subject_type="listing",
                subject_id=listing_id, request_id_value=request_id(request),
                metadata={"listingVersionId": str(published.current_published_version_id)},
            )
            await idempotency_service.complete(connection, claim, response_status=200, response_body=body, response_headers=headers)
        return body, 200, headers

    async def unpublish(
        self, *, principal: AccessPrincipal, listing_id: UUID, idempotency_key: str, request: Request
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope=f"marketplace.unpublish_listing:{listing_id}", key=idempotency_key,
                body={"listingId": str(listing_id)},
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 200, claim.replay_headers or {}
            current = await repository.lock_owned(connection, listing_id=listing_id, creator_id=principal.user_id)
            if current is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="listing_not_found")
            if current.status != "published":
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="listing_not_unpublishable")
            unpublished = await repository.unpublish(connection, listing=current)
            view = await _listing_view(unpublished, self._assets)
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="listing.unpublished", subject_type="listing",
                subject_id=listing_id, request_id_value=request_id(request),
            )
            await idempotency_service.complete(connection, claim, response_status=200, response_body=body, response_headers=headers)
        return body, 200, headers

    async def get_listing(self, *, listing_id: UUID, principal: AccessPrincipal | None) -> ListingView:
        async with get_engine().connect() as connection:
            record = await repository.find_published(connection, listing_id=listing_id)
            if record is None and principal is not None:
                record = await repository.find_owned(connection, listing_id=listing_id, creator_id=principal.user_id)
        if record is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="listing_not_found")
        return await _listing_view(record, self._assets)

    async def explore(
        self, *, q: str | None, tag: str | None, creator: str | None, media_type: str | None,
        min_price: Decimal | None, max_price: Decimal | None,
        sort: Literal["newest", "price_asc", "price_desc", "relevance"], after: dict[str, object] | None, limit: int,
    ) -> tuple[list[ListingView], list[repository.ListingRecord]]:
        async with get_engine().connect() as connection:
            records = await repository.search_published(
                connection, q=q, tag=tag, creator=creator, media_type=media_type, min_price=min_price,
                max_price=max_price, sort=sort, after=after, limit=limit,
            )
        return list(await asyncio.gather(*(_listing_view(record, self._assets) for record in records))), records

    async def list_creator(
        self, *, principal: AccessPrincipal, listing_status: str | None, limit: int,
        before: tuple[datetime, UUID] | None,
    ) -> tuple[list[ListingView], list[repository.ListingRecord]]:
        async with get_engine().connect() as connection:
            records = await repository.list_creator(
                connection, creator_id=principal.user_id, status=listing_status, limit=limit,
                before_created_at=before[0] if before else None, before_id=before[1] if before else None,
            )
        return list(await asyncio.gather(*(_listing_view(record, self._assets) for record in records))), records

    async def save_favorite(
        self, *, principal: AccessPrincipal, asset_id: UUID, idempotency_key: str, request: Request
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope=f"marketplace.favorite:{asset_id}", key=idempotency_key,
                body={"assetId": str(asset_id)},
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 201, claim.replay_headers or {}
            favorite = await repository.save_favorite(connection, user_id=principal.user_id, asset_id=asset_id)
            if favorite is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="published_asset_not_found")
            view = await self._favorite_view(favorite)
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="asset.favorited", subject_type="asset",
                subject_id=asset_id, request_id_value=request_id(request),
            )
            await idempotency_service.complete(connection, claim, response_status=201, response_body=body, response_headers=headers)
        return body, 201, headers

    async def remove_favorite(
        self, *, principal: AccessPrincipal, asset_id: UUID, idempotency_key: str, request: Request
    ) -> tuple[int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope=f"marketplace.unfavorite:{asset_id}", key=idempotency_key,
                body={"assetId": str(asset_id)},
            )
            if claim.is_replay:
                return claim.replay_status or 204, claim.replay_headers or {}
            await repository.delete_favorite(connection, user_id=principal.user_id, asset_id=asset_id)
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="asset.unfavorited", subject_type="asset",
                subject_id=asset_id, request_id_value=request_id(request),
            )
            await idempotency_service.complete(connection, claim, response_status=204, response_body=None, response_headers=headers)
        return 204, headers

    async def list_favorites(
        self, *, principal: AccessPrincipal, limit: int, before: tuple[datetime, UUID] | None
    ) -> list[FavoriteView]:
        async with get_engine().connect() as connection:
            records = await repository.list_favorites(connection, user_id=principal.user_id, limit=limit, before=before)
        return list(await asyncio.gather(*(self._favorite_view(record) for record in records)))

    async def find_published_offer(
        self, *, listing_id: UUID, licence_offer_id: UUID
    ) -> PublishedOfferSnapshot | None:
        async with get_engine().connect() as connection:
            return await repository.find_published_offer(connection, listing_id=listing_id, licence_offer_id=licence_offer_id)

    def _resolve_update_licence(
        self, offer: LicenceOfferInput | None
    ) -> tuple[str, str, dict[str, object]] | None:
        if offer is None:
            return None
        return offer.code, offer.terms_version, self._licences.resolve_immutable_terms(
            code=offer.code, terms_version=offer.terms_version
        )

    @staticmethod
    def _snapshot(record: repository.ListingRecord, *, media_type: str) -> dict[str, object]:
        return {
            "listingId": str(record.id), "assetId": str(record.asset_id), "creatorId": str(record.creator_id),
            "creator": {"handle": record.creator_handle, "displayName": record.creator_display_name},
            "title": record.title, "description": record.description, "tags": record.tags,
            "price": format(record.price, ".2f"), "currency": record.currency, "mediaType": media_type,
            "licenceOffer": {"id": str(record.licence_offer_id), "code": record.licence_code,
                              "termsVersion": record.licence_terms_version, "terms": record.licence_terms},
        }

    async def _favorite_view(self, favorite: repository.FavoriteRecord) -> FavoriteView:
        preview = await self._assets.find_public_preview(asset_id=favorite.asset_id)
        listing = await _listing_view(favorite.listing, self._assets) if favorite.listing is not None else None
        return FavoriteView(assetId=favorite.asset_id, createdAt=favorite.created_at, preview=_preview_view(preview), listing=listing)
