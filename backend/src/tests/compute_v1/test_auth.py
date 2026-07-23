from __future__ import annotations

from .client import ComputeClient


def test_capabilities_require_service_auth(client: ComputeClient) -> None:
    result = client.request("/compute/v1/capabilities", authorized=False)

    assert result.status == 401
    assert result.json()["error"]["code"] == "COMPUTE_UNAUTHORIZED"


def test_legacy_native_compiler_is_disabled(client: ComputeClient) -> None:
    result = client.request(
        "/api/variants/compile",
        body={"name": "forbidden", "formula": "z*z+c"},
        authorized=False,
    )

    assert result.status == 403
    assert result.json()["error"]["code"] == "LEGACY_FORMULA_COMPILER_DISABLED"
