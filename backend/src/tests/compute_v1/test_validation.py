from __future__ import annotations

from copy import deepcopy
from typing import Any

import pytest

from .client import ComputeClient
from .payloads import map_payload, sequence_program, strict_dsl_payload


def test_invalid_dsl_returns_unknown_function(client: ComputeClient) -> None:
    payload = map_payload()
    payload["orbitProgram"] = {
        "type": "formula",
        "formula": {"type": "dsl", "source": "system(z)"},
    }

    result = client.preview("map_image", payload)

    assert result.status == 400
    assert result.json()["error"]["code"] == "UNKNOWN_FUNCTION"


def test_parameterized_dsl_accepts_real_and_complex_object_values(
    client: ComputeClient,
) -> None:
    payload = map_payload()
    payload["orbitProgram"] = {
        "type": "formula",
        "formula": {
            "type": "dsl", "source": "z*z+c+a*sin(z)+shift",
            "parameters": {"a": 0.12, "shift": {"re": 0.01, "im": -0.02}},
        },
    }

    result = client.preview("map_image", payload)

    assert result.status == 200
    assert result.headers["X-FSD-Engine"] == "openmp"
    assert result.headers["X-FSD-Scalar"] == "fp64"


def validation_cases() -> list[pytest.ParameterSet]:
    strict = strict_dsl_payload()
    invalid_metric = deepcopy(strict)
    invalid_metric["metric"] = "min_abs"
    invalid_axis = map_payload(orbit=True)
    invalid_axis["transitionTheta"] = 45.0
    return [
        pytest.param(
            {"schemaVersion": 2, "kind": "raw_field", "payload": strict},
            400, "UNSUPPORTED_SCHEMA_VERSION", id="schema-version",
        ),
        pytest.param(
            {"schemaVersion": 1, "kind": "raw_field", "payload": invalid_metric},
            422, "UNSUPPORTED_CAPABILITY", id="raw-field-metric",
        ),
        pytest.param(
            {"schemaVersion": 1, "kind": "transition_video_preview",
             "payload": {"orbitProgram": sequence_program()}},
            422, "UNSUPPORTED_CAPABILITY", id="transition-orbit",
        ),
        pytest.param(
            {"schemaVersion": 1, "kind": "map_image", "payload": invalid_axis},
            422, "UNSUPPORTED_CAPABILITY", id="axis-on-map",
        ),
    ]


@pytest.mark.parametrize("envelope,status,code", validation_cases())
def test_unsupported_requests_return_structured_errors(
    client: ComputeClient, envelope: dict[str, Any], status: int, code: str,
) -> None:
    result = client.request("/compute/v1/previews", body=envelope)

    assert result.status == status
    assert result.json()["error"]["code"] == code
    assert isinstance(result.json()["error"]["details"], dict)
