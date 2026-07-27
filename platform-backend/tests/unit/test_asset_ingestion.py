"""T07 strict artifact-boundary checks; I/O behaviour covered by dev-stack smoke flow."""

from __future__ import annotations

from uuid import uuid4

import pytest

from app.assets.service import AssetIngestionService
from app.infrastructure.compute.compute_artifact_reader import ArtifactIntegrityError, ComputeArtifactReader


_SHA = "a" * 64


def _metadata(*, artifact_id: str = "run-1:master.png", media_type: str = "image/png") -> dict[str, object]:
    return {
        "artifacts": [
            {
                "artifactId": artifact_id,
                "mediaType": media_type,
                "sha256": _SHA,
                "sizeBytes": 1,
            }
        ]
    }


def test_selected_allowed_artifact_uses_required_private_key_shape() -> None:
    artifact = AssetIngestionService._master_metadata(_metadata(), ["run-1:master.png"])
    asset_id = uuid4()
    file_id = uuid4()
    assert AssetIngestionService._master_object_key(asset_id, file_id, artifact["filename"]) == (
        f"private/masters/{asset_id}/{file_id}/master.png"
    )


@pytest.mark.parametrize(
    "artifact_id",
    ["master.png", "../run:master.png", "run-1:../master.png", "run-1:dir/master.png", "run-1:master.jpg", "run-1:one:two.png"],
)
def test_rejects_malformed_or_non_allowlisted_master(artifact_id: str) -> None:
    with pytest.raises(ArtifactIntegrityError):
        AssetIngestionService._master_metadata(_metadata(artifact_id=artifact_id), [artifact_id])


def test_rejects_artifact_not_selected_by_render_worker() -> None:
    with pytest.raises(ArtifactIntegrityError, match="selected_artifact_not_found"):
        AssetIngestionService._master_metadata(_metadata(), ["run-1:other.png"])


def test_reader_rejects_path_traversal_before_network() -> None:
    with pytest.raises(ArtifactIntegrityError, match="invalid_artifact_id"):
        ComputeArtifactReader.validate_artifact_id("run-1:../../secret.png")
