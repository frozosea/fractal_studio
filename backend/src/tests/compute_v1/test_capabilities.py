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
