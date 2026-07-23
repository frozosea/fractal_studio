from __future__ import annotations

from .client import ComputeClient
from .payloads import map_payload


def test_running_map_can_be_cancelled(client: ComputeClient) -> None:
    payload = map_payload(orbit=True, size=1024, iterations=100_000)
    run_id, _ = client.create_run("map_image", payload)

    cancellation = client.cancel(run_id)
    terminal = client.wait_for_run(run_id)

    assert cancellation["accepted"] is True
    assert terminal["status"] == "cancelled"
