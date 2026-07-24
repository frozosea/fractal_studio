"""Small S3/MinIO adapter. Object keys stay owned by Platform domain services."""

from __future__ import annotations

import asyncio
from pathlib import Path

import boto3

from app.core.config import Settings, get_settings


class ObjectStorage:
    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    def _client(self):
        return boto3.client(
            "s3",
            endpoint_url=self._settings.s3_endpoint_url or None,
            region_name=self._settings.s3_region,
            aws_access_key_id=self._settings.s3_access_key_id or None,
            aws_secret_access_key=self._settings.s3_secret_access_key or None,
        )

    def _public_client(self):
        return boto3.client(
            "s3",
            endpoint_url=self._settings.s3_public_endpoint_url or self._settings.s3_endpoint_url or None,
            region_name=self._settings.s3_region,
            aws_access_key_id=self._settings.s3_access_key_id or None,
            aws_secret_access_key=self._settings.s3_secret_access_key or None,
        )

    async def upload_file(self, *, object_key: str, source: Path, media_type: str) -> None:
        client = self._client()
        await asyncio.to_thread(
            client.upload_file,
            str(source),
            self._settings.s3_bucket,
            object_key,
            ExtraArgs={"ContentType": media_type},
        )

    async def upload_bytes(self, *, object_key: str, body: bytes, media_type: str) -> None:
        """Upload generated metadata without exposing local files to callers."""
        client = self._client()
        await asyncio.to_thread(
            client.put_object,
            Bucket=self._settings.s3_bucket,
            Key=object_key,
            Body=body,
            ContentType=media_type,
        )

    async def download_file(self, *, object_key: str, destination: Path) -> None:
        client = self._client()
        await asyncio.to_thread(
            client.download_file,
            self._settings.s3_bucket,
            object_key,
            str(destination),
        )

    async def create_signed_get_url(self, *, object_key: str, expires_seconds: int) -> str:
        client = self._public_client()
        return await asyncio.to_thread(
            client.generate_presigned_url,
            "get_object",
            Params={"Bucket": self._settings.s3_bucket, "Key": object_key},
            ExpiresIn=expires_seconds,
        )

    async def delete(self, *, object_key: str) -> None:
        client = self._client()
        await asyncio.to_thread(client.delete_object, Bucket=self._settings.s3_bucket, Key=object_key)
