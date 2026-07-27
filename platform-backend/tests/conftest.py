"""Shared test fixtures."""

import os

import pytest


@pytest.fixture
def e2e_api_url() -> str:
    """Explicit opt-in keeps regular unit runs independent from local Compose."""
    if endpoint := os.getenv("E2E_API_URL"):
        return endpoint
    pytest.skip("set E2E_API_URL to run live Compose E2E checks")
