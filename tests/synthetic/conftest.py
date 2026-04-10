import sys
import os
import pytest

# Add project root to path so tests can import from tests/mock/
_PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
if _PROJECT_ROOT not in sys.path:
    sys.path.insert(0, _PROJECT_ROOT)

from tests.mock.la_synthetic_device import LaSyntheticDevice


@pytest.fixture
def synthetic_device() -> LaSyntheticDevice:
    """Return a fresh LaSyntheticDevice instance."""
    return LaSyntheticDevice()
