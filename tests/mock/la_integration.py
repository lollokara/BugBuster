"""Bridge for LaSyntheticDevice in HAT LA handlers."""
from tests.mock.la_synthetic_device import LaSyntheticDevice


def generate_la_oneshot(n_channels=4, n_samples=100):
    return LaSyntheticDevice().generate_oneshot(n_channels=n_channels, n_samples=n_samples)
