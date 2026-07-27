"""Verified private Compute v1 artifact downloader."""

from __future__ import annotations

import hashlib
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path

from app.infrastructure.compute.compute_client import ComputeClient, ComputeClientError


class ArtifactIntegrityError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class VerifiedArtifact:
    path: Path
    sha256: str
    size_bytes: int


class ComputeArtifactReader:
    def __init__(self, client: ComputeClient | None = None) -> None:
        self._client = client or ComputeClient()

    async def download_verified(
        self, *, artifact_id: str, expected_sha256: str, expected_size_bytes: int
    ) -> VerifiedArtifact:
        self.validate_artifact_id(artifact_id)
        if len(expected_sha256) != 64 or any(char not in "0123456789abcdef" for char in expected_sha256):
            raise ArtifactIntegrityError("invalid_expected_sha256")
        if not 0 < expected_size_bytes <= 524_288_000:
            raise ArtifactIntegrityError("invalid_expected_size")
        descriptor, path_text = tempfile.mkstemp(prefix="fractal-artifact-", suffix=".bin")
        path = Path(path_text)
        digest = hashlib.sha256()
        size = 0
        try:
            with os.fdopen(descriptor, "wb") as target:
                async for chunk in self._client.stream_artifact(artifact_id=artifact_id):
                    size += len(chunk)
                    if size > expected_size_bytes:
                        raise ArtifactIntegrityError("artifact_too_large")
                    digest.update(chunk)
                    target.write(chunk)
            actual = digest.hexdigest()
            if size != expected_size_bytes or actual != expected_sha256:
                raise ArtifactIntegrityError("artifact_checksum_mismatch")
            return VerifiedArtifact(path=path, sha256=actual, size_bytes=size)
        except (ArtifactIntegrityError, ComputeClientError):
            path.unlink(missing_ok=True)
            raise
        except Exception:
            path.unlink(missing_ok=True)
            raise

    @staticmethod
    def validate_artifact_id(artifact_id: str) -> tuple[str, str]:
        """Allow only opaque Compute IDs shaped exactly as ``runId:fileName``."""
        run_id, separator, filename = artifact_id.partition(":")
        if (
            not separator
            or not run_id
            or not filename
            or ":" in filename
            or ".." in run_id
            or "/" in run_id
            or "\\" in run_id
            or filename in {".", ".."}
            or ".." in filename
            or "/" in filename
            or "\\" in filename
            or Path(filename).name != filename
        ):
            raise ArtifactIntegrityError("invalid_artifact_id")
        return run_id, filename
