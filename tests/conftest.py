"""pytest setup for the integration tests (pytest-homeassistant-custom-component).

The registry tests are plain Python and run without Home Assistant installed; the
autouse fixture below only turns custom integrations on when the test kit is present.
"""
import importlib.util

import pytest

if importlib.util.find_spec("pytest_homeassistant_custom_component"):

    @pytest.fixture(autouse=True)
    def auto_enable_custom_integrations(enable_custom_integrations):
        yield
