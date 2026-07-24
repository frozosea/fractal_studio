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
            ExtraArgs=self._upload_args(object_key=object_key, media_type=media_type),
        )

    async def upload_bytes(self, *, object_key: str, body: bytes, media_type: str) -> None:
        """Upload generated metadata without exposing local files to callers."""
        client = self._client()
        await asyncio.to_thread(
            client.put_object,
            Bucket=self._settings.s3_bucket,
            Key=object_key,
            Body=body,
            **self._put_args(object_key=object_key, media_type=media_type),
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

    def _upload_args(self, *, object_key: str, media_type: str) -> dict[str, str]:
        return {"ContentType": media_type, **self._storage_policy_args(object_key)}

    def _put_args(self, *, object_key: str, media_type: str) -> dict[str, str]:
        return {"ContentType": media_type, **self._storage_policy_args(object_key)}

    def _storage_policy_args(self, object_key: str) -> dict[str, str]:
        """Prefix-based cache and encryption policy. Access remains signed, never ACL based."""
        if object_key.startswith("public/previews/"):
            args: dict[str, str] = {"CacheControl": "public, max-age=3600"}
        else:
            args = {"CacheControl": "private, no-store"}
        encryption = self._settings.s3_server_side_encryption
        if encryption:
            args["ServerSideEncryption"] = encryption
            if encryption == "aws:kms" and self._settings.s3_sse_kms_key_id:
                args["SSEKMSKeyId"] = self._settings.s3_sse_kms_key_id
        return args
