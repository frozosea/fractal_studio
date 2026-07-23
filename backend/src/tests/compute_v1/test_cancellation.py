from __future__ import annotations

from .client import ComputeClient
from .payloads import (
    hs_field_payload, hs_mesh_payload, ln_map_payload, map_payload, sequence_program,
    transition_mesh_payload,
    transition_voxels_payload,
    special_points_enumerate_payload,
)


def test_running_map_can_be_cancelled(client: ComputeClient) -> None:
    payload = map_payload(orbit=True, size=1024, iterations=100_000)
    run_id, _ = client.create_run("map_image", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"


def test_running_ln_map_can_be_cancelled(client: ComputeClient) -> None:
    payload = ln_map_payload(orbit=sequence_program())
    payload.update({"widthS": 1024, "depthOctaves": 10.0, "iterations": 100_000})
    run_id, _ = client.create_run("ln_map", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"


def test_running_hs_mesh_can_be_cancelled(client: ComputeClient) -> None:
    payload = hs_mesh_payload()
    payload.update({"resolution": 512, "iterations": 100_000})
    run_id, _ = client.create_run("hs_mesh", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"


def test_running_hs_field_can_be_cancelled(client: ComputeClient) -> None:
    payload = hs_field_payload()
    payload.update({"resolution": 512, "iterations": 100_000})
    run_id, _ = client.create_run("hs_field", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"


def test_running_transition_mesh_can_be_cancelled(client: ComputeClient) -> None:
    payload = transition_mesh_payload()
    payload.update({"resolution": 128, "iterations": 10_000})
    run_id, _ = client.create_run("transition_mesh", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"


def test_running_transition_voxels_can_be_cancelled(client: ComputeClient) -> None:
    payload = transition_voxels_payload()
    payload.update({"resolution": 128, "iterations": 10_000})
    run_id, _ = client.create_run("transition_voxels", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"


def test_special_points_enumeration_can_be_cancelled(client: ComputeClient) -> None:
    payload = special_points_enumerate_payload()
    payload.update({"periodMax": 8, "maxSeedBatches": 200, "seedsPerBatch": 10_000})
    run_id, _ = client.create_run("special_points_enumerate", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"
