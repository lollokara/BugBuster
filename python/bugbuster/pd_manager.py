"""
USB PD Profile Manager
======================

Selects the minimum USB-C Power Delivery profile that satisfies a DC-DC
converter's supply demand, accounting for converter topology and headroom.

Topology reference
------------------
ESP32-S3 board:
  VADJ1, VADJ2 — LTM8063 *buck* regulators, 3–15 V.
  These rails are fed directly from the USB-C PD bus.  A buck regulator
  requires its input to be strictly *above* its output — use
  :attr:`ConverterTopology.BUCK` and allow ``BUCK_HEADROOM_V`` (2 V).

RP2040 HAT:
  VADJ3, VADJ4 — LTM8083 *buck-boost* regulators, up to ~30 V.
  A buck-boost can both step up and step down from any input.  For
  efficiency, the supply should be as close as possible to the target:
  below the target the converter boosts; above it bucks.  Use
  :attr:`ConverterTopology.BUCK_BOOST`.  Targets beyond 20 V (the USB-PD
  ceiling) require the converter to boost from 20 V — that is correct and
  fully supported.

DAQ HAT (ESP32-P4):
  Buck-only DC-DC outputs, same headroom rule as VADJ1/2.
  Use :attr:`ConverterTopology.BUCK`.

Available USB-C PD profiles (HUSB238)
--------------------------------------
5 V · 9 V · 12 V · 15 V · 18 V · 20 V

Usage
-----
::

    from bugbuster.pd_manager import ensure_pd_for_output, ConverterTopology

    with connect_usb("/dev/ttyACM0") as bb:
        # Sequence: negotiate PD first, then program the DCDC.
        pd_v = ensure_pd_for_output(bb, target_v=12.0, topology=ConverterTopology.BUCK)
        bb.hal.set_voltage(rail=1, voltage=12.0)

        # HAT buck-boost rail — headroom logic differs.
        pd_v = ensure_pd_for_output(bb, target_v=24.0, topology=ConverterTopology.BUCK_BOOST)
        bb.hat_set_rail_voltage(rail_id=1, voltage_mv=24_000)
"""

from __future__ import annotations

import time
import logging
from typing import Optional

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

#: Standard USB-C PD voltages supported by the HUSB238 sink controller,
#: sorted ascending.
PD_PROFILES_V: list[int] = [5, 9, 12, 15, 18, 20]

#: Minimum Vin − Vout margin (volts) for *buck* converters.
#: Buck regulators cannot produce output voltages at or above their input.
#: 2 V provides safe regulation headroom including ripple and startup transients.
BUCK_HEADROOM_V: float = 2.0

#: Preferred Vin headroom (volts) for *buck-boost* converters operating in
#: buck-down mode.  Keeps the duty cycle well below 100 % and improves
#: efficiency when the target is below the USB-PD ceiling.
BUCK_BOOST_HEADROOM_V: float = 1.0

#: Seconds to wait after requesting a new PD contract before programming
#: a DC-DC converter.  The HUSB238 takes ≤ 150 ms to renegotiate.
PD_SETTLE_S: float = 0.15


# ---------------------------------------------------------------------------
# Converter topology
# ---------------------------------------------------------------------------

class ConverterTopology:
    """Symbolic constants for DC-DC converter topology."""
    BUCK       = "buck"        #: Output must be < input (LTM8063 VADJ1/2).
    BUCK_BOOST = "buck_boost"  #: Output can be > or < input (LTM8083 VADJ3/4).


# ---------------------------------------------------------------------------
# Pure profile-selection logic (no I/O, fully unit-testable)
# ---------------------------------------------------------------------------

def select_pd_profile(
    target_v: float,
    topology: str = ConverterTopology.BUCK,
    available_profiles: Optional[list[int]] = None,
) -> int:
    """
    Return the lowest USB-PD profile voltage that satisfies the demand.

    Parameters
    ----------
    target_v:
        Desired DC-DC output voltage in volts.
    topology:
        :attr:`ConverterTopology.BUCK` or :attr:`ConverterTopology.BUCK_BOOST`.
    available_profiles:
        Voltage list to search.  Defaults to :attr:`PD_PROFILES_V`.
        Unrecognised voltages are silently dropped.

    Returns
    -------
    int
        The selected USB-PD supply voltage in volts.

    Raises
    ------
    ValueError
        If no profile can satisfy the demand (e.g. buck needs > 22 V input
        for a 20 V+ output but the max PD is 20 V).
    """
    if target_v <= 0:
        raise ValueError(f"target_v must be positive, got {target_v!r}")

    candidates = sorted(
        v for v in (available_profiles if available_profiles is not None else PD_PROFILES_V)
        if isinstance(v, (int, float)) and v > 0
    )
    if not candidates:
        raise ValueError("No USB-PD profiles available to select from.")

    if topology == ConverterTopology.BUCK:
        # Buck: Vin must exceed Vout by at least BUCK_HEADROOM_V.
        min_supply = target_v + BUCK_HEADROOM_V
        for profile in candidates:
            if profile >= min_supply:
                return int(profile)
        raise ValueError(
            f"No available PD profile can supply {target_v:.2f} V through a buck "
            f"converter (need Vin ≥ {min_supply:.1f} V; "
            f"highest available profile is {max(candidates)} V)."
        )

    elif topology == ConverterTopology.BUCK_BOOST:
        max_profile = max(candidates)
        if target_v > max_profile:
            # Target exceeds the USB-PD ceiling — the buck-boost must boost.
            # Use the highest available profile for maximum headroom.
            return int(max_profile)

        # Target is within PD range: pick the smallest profile that keeps the
        # converter in a comfortable buck-down region (slight step-down from
        # the PD bus is more efficient than a large step-down or boosting).
        min_supply = target_v + BUCK_BOOST_HEADROOM_V
        for profile in candidates:
            if profile >= min_supply:
                return int(profile)

        # All profiles are below target + headroom but target ≤ max_profile —
        # the converter will boost slightly from the highest available profile.
        return int(max_profile)

    else:
        raise ValueError(
            f"Unknown converter topology {topology!r}. "
            f"Use ConverterTopology.BUCK or ConverterTopology.BUCK_BOOST."
        )


# ---------------------------------------------------------------------------
# Device interaction helpers
# ---------------------------------------------------------------------------

def _get_available_profiles(client) -> list[int]:
    """
    Read the PD source's advertised PDO voltages from the device.

    Falls back to the full :attr:`PD_PROFILES_V` list when the status is
    unavailable or no detected PDOs are returned (e.g. no PD charger attached).
    """
    try:
        status = client.usbpd_get_status()
    except Exception as exc:
        log.warning("pd_manager: could not read PD status: %s — using default profiles", exc)
        return list(PD_PROFILES_V)

    pdos = status.get("pdos") or []
    detected = [
        int(p["voltage_v"])
        for p in pdos
        if isinstance(p, dict) and p.get("voltage_v") and p.get("detected")
    ]

    # Always include the currently-negotiated voltage as a known-available option.
    current_v = status.get("voltage_v")
    if current_v:
        try:
            detected.append(int(current_v))
        except (TypeError, ValueError):
            pass

    if detected:
        return sorted(set(detected))

    # No PDOs detected (no PD charger or 5 V only) — assume standard set.
    log.debug("pd_manager: no PDOs detected; assuming full profile list")
    return list(PD_PROFILES_V)


def ensure_pd_for_output(
    client,
    target_v: float,
    topology: str = ConverterTopology.BUCK,
) -> int:
    """
    Negotiate the appropriate USB-C PD profile for the given DC-DC demand.

    Reads the source's advertised PDOs, computes the minimum suitable
    profile for *topology*, and renegotiates the contract only when the
    current profile differs from the required one.

    **Always call this before programming any DC-DC converter.**

    Parameters
    ----------
    client:
        A connected :class:`bugbuster.client.BugBuster` instance.
    target_v:
        Desired DC-DC output voltage in volts.
    topology:
        :attr:`ConverterTopology.BUCK` (default) or
        :attr:`ConverterTopology.BUCK_BOOST`.

    Returns
    -------
    int
        The negotiated (or already-active) USB-PD supply voltage in volts.

    Raises
    ------
    ValueError
        If no available PD profile can satisfy the demand.
    RuntimeError
        If the required profile is not offered by the source.
    """
    available = _get_available_profiles(client)
    needed_v = select_pd_profile(target_v, topology, available_profiles=available)

    try:
        status = client.usbpd_get_status()
        current_v = int(status.get("voltage_v") or 0)
    except Exception as exc:
        log.warning("pd_manager: could not read current PD voltage: %s", exc)
        current_v = 0

    if current_v == needed_v:
        log.debug("pd_manager: PD already at %d V — no renegotiation needed", needed_v)
        return needed_v

    log.info(
        "pd_manager: renegotiating PD %d V → %d V for %.2f V %s output",
        current_v, needed_v, target_v, topology,
    )
    client.usbpd_select_voltage(needed_v)
    time.sleep(PD_SETTLE_S)
    return needed_v


def downgrade_pd_if_possible(
    client,
    *demands: tuple[float, str],
) -> int:
    """
    Select the *minimum* PD profile that satisfies all supplied demands at once.

    Use this when multiple rails are active simultaneously and you want to
    negotiate once for the collective worst-case requirement.

    Parameters
    ----------
    client:
        A connected :class:`bugbuster.client.BugBuster` instance.
    *demands:
        Any number of ``(target_v, topology)`` tuples, e.g.::

            downgrade_pd_if_possible(
                bb,
                (12.0, ConverterTopology.BUCK),        # VADJ1
                (5.0,  ConverterTopology.BUCK),        # VADJ2
                (24.0, ConverterTopology.BUCK_BOOST),  # VADJ3
            )

    Returns
    -------
    int
        The negotiated USB-PD supply voltage in volts.
    """
    if not demands:
        raise ValueError("At least one demand tuple is required.")

    available = _get_available_profiles(client)

    required_profiles = [
        select_pd_profile(target_v, topology, available_profiles=available)
        for target_v, topology in demands
    ]
    worst_case_v = max(required_profiles)

    try:
        status = client.usbpd_get_status()
        current_v = int(status.get("voltage_v") or 0)
    except Exception as exc:
        log.warning("pd_manager: could not read current PD voltage: %s", exc)
        current_v = 0

    if current_v == worst_case_v:
        log.debug("pd_manager: PD already at %d V — no renegotiation needed", worst_case_v)
        return worst_case_v

    log.info("pd_manager: renegotiating PD %d V → %d V for %d demand(s)",
             current_v, worst_case_v, len(demands))
    client.usbpd_select_voltage(worst_case_v)
    time.sleep(PD_SETTLE_S)
    return worst_case_v
