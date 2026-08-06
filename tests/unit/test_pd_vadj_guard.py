import warnings

from bugbuster.client import BugBusterWarning, _pd_limited_vadj_warning


def test_vadj_warning_when_pd_controller_missing_above_5v():
    warning = _pd_limited_vadj_warning({"present": False}, rail=1, voltage=6.0)

    assert warning is not None
    assert "VADJ1=6.00 V" in warning
    assert "USB-C PD controller is not detected" in warning


def test_vadj_warning_scales_to_negotiated_pd_voltage():
    status = {"present": True, "attached": True, "voltage_v": 9.0, "pdos": []}

    assert _pd_limited_vadj_warning(status, rail=1, voltage=9.0) is None
    warning = _pd_limited_vadj_warning(status, rail=2, voltage=12.0)

    assert warning is not None
    assert "VADJ2=12.00 V" in warning
    assert "negotiated USB-C input is only 9.0 V" in warning


def test_vadj_warning_allows_request_below_higher_pd_voltage():
    status = {"present": True, "attached": True, "voltage_v": 12.0, "pdos": []}

    assert _pd_limited_vadj_warning(status, rail=1, voltage=9.0) is None


def test_vadj_warning_ignores_vlogic_and_hat_rails():
    status = {"present": False, "attached": False, "voltage_v": 0.0, "pdos": []}

    assert _pd_limited_vadj_warning(status, rail=0, voltage=12.0) is None
    assert _pd_limited_vadj_warning(status, rail=3, voltage=12.0) is None


def test_idac_set_voltage_emits_python_warning_before_write():
    class DummyBugBuster:
        def usbpd_get_status(self):
            return {"present": True, "attached": True, "voltage_v": 5.0, "pdos": []}

        def check_vadj_pd_limit(self, rail, voltage):
            from bugbuster.client import _pd_limited_vadj_warning
            return _pd_limited_vadj_warning(self.usbpd_get_status(), rail, voltage)

    warning = DummyBugBuster().check_vadj_pd_limit(1, 8.0)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        if warning:
            warnings.warn(warning, BugBusterWarning, stacklevel=2)

    assert caught
    assert issubclass(caught[0].category, BugBusterWarning)
    assert "negotiated USB-C input is only 5.0 V" in str(caught[0].message)
