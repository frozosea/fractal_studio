"""Production Compose must keep AI provider credentials inside the API boundary."""

from __future__ import annotations

from pathlib import Path

import yaml


def test_ai_provider_keys_are_masked_outside_the_api() -> None:
    compose_path = Path(__file__).parents[3] / "ops" / "production" / "docker-compose.vps.yml"
    services = yaml.safe_load(compose_path.read_text())["services"]

    services_with_shared_env = {
        name: service
        for name, service in services.items()
        if "env_file" in service
    }
    assert "api" in services_with_shared_env

    for name, service in services_with_shared_env.items():
        environment = service.get("environment", {})
        if name == "api":
            assert environment["SILICONFLOW_API_KEY"] != ""
            assert environment["DEEPSEEK_API_KEY"] != ""
            continue
        assert environment.get("SILICONFLOW_API_KEY") == "", name
        assert environment.get("DEEPSEEK_API_KEY") == "", name
