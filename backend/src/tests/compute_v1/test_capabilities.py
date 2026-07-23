from __future__ import annotations

from .client import ComputeClient


def test_capabilities_advertise_implemented_orbit_contract(client: ComputeClient) -> None:
    result = client.request("/compute/v1/capabilities")

    assert result.status == 200
    capabilities = result.json()
    assert "map_image" in capabilities["persistentKinds"]
    assert capabilities["orbitPrograms"]["sequence"] is True
    assert capabilities["customFormula"]["safeDsl"] is True
    assert capabilities["customFormula"]["legacyNativeCompile"] is False
    assert capabilities["escapeSemantics"]["strictUnverified"] is True
    assert capabilities["orbitCompatibility"]["lnMap"] is True
    assert capabilities["orbitCompatibility"]["zoomVideo"] is True


def test_capabilities_report_runtime_hardware(client: ComputeClient) -> None:
    capabilities = client.request("/compute/v1/capabilities").json()

    hardware = capabilities["hardware"]
    assert hardware["cpu"]["logicalCores"] >= 1
    assert isinstance(hardware["cpu"]["openmp"]["compiled"], bool)
    assert isinstance(hardware["cpu"]["openmp"]["runtime"], bool)
    assert isinstance(hardware["cuda"]["compiled"], bool)
    assert isinstance(hardware["cuda"]["runtime"], bool)
    assert hardware["cuda"]["deviceCount"] >= 0
