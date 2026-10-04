"""DAQ HAT control from on-device scripts: the VDUT programmable supply and the
battery simulator. Every call raises ``OSError(ENODEV)`` when no DAQ HAT is
connected; HAT link failures raise ``OSError(EIO)``; battery-simulator refusals
raise ``RuntimeError`` naming the simulator's reason (e.g. ``"busy"``).

    import daq
    if daq.present():
        daq.vdut(True, volts=3.3, amps_limit=0.5)
        print(daq.read())
"""

def present() -> bool:
    """True when a DAQ HAT is connected (never raises)."""

def vdut(enable: bool | None = None, *, volts: float | None = None,
         amps_limit: float | None = None) -> dict:
    """Set any of the given VDUT values, then return the supply state.

    ``volts`` 1.76-19.94 V and ``amps_limit`` 0.05-2.636 A (``ValueError``
    outside). The setpoint is applied before ``enable``. Returns
    ``{"present", "enabled", "fault", "volts", "amps_limit", "v", "i"}`` with
    setpoints in ``volts``/``amps_limit`` and measurements in ``v``/``i`` (V, A).
    """

def read() -> dict:
    """Measured DUT output: ``{"v": V, "i": A, "p": W, "enabled": bool}``."""

def samples(run_id: int, since_s: int = 0, max: int = 600) -> list[tuple[int, float, float, float, int]]:
    """Live 1 s battery-run samples newer than ``since_s`` (simulated seconds).

    Tuples are ``(t_s, v, i, soc, flags)``: interval end, mean V, mean A, SOC %,
    and the history ``RF_*`` flags (1 gap, 2 current clamp, 4 resume, 8 output
    off, 16 below cutoff). Only the loaded run's last hour is available;
    another run raises ``OSError(ENOENT)``. ``max`` is 1-3600.
    """

class run:
    """Battery-simulator runs. A namespace: call as ``daq.run.status()``."""

    @staticmethod
    def status() -> dict:
        """Loaded run: ``{"run_id", "state", "soc", "v", "i", "elapsed_s",
        "remaining_s", "energy_j", "error", "name", "params"}``. ``state`` is
        none/paused/active/depleted/stopped; ``params`` is the dict accepted by
        :func:`edit` (``None`` when no run is loaded)."""

    @staticmethod
    def list() -> list[dict]:
        """Every stored run: ``{"run_id", "name", "created", "chem", "cells",
        "capacity_mah", "active"}``."""

    @staticmethod
    def new(name: str, chem: str | int, cells: int, capacity_mah: int, soc: float = 100,
            self_discharge: float | None = None, ext_load: int | None = None) -> int:
        """Create and load a run (paused) and return its id.

        ``name`` <= 23 bytes; ``chem`` 'lipo' | 'lifepo4' | 'nimh' | 'lead';
        ``soc`` start state of charge %; ``self_discharge`` %/month (None = off);
        ``ext_load`` extra virtual load in µA (None = off).
        """

    @staticmethod
    def start(run_id: int | None = None) -> None:
        """Start (or resume) integrating; loads ``run_id`` first if given."""

    @staticmethod
    def pause() -> None:
        """Output off, everything frozen."""

    @staticmethod
    def stop() -> None:
        """Finalise the run (kept on flash)."""

    @staticmethod
    def reopen(run_id: int | None = None) -> None:
        """Resume a ``stopped`` run: back to ``paused`` (output off); call
        :func:`start` to continue. Loads ``run_id`` first if given. Raises
        ``RuntimeError`` for a ``depleted`` run (cutoff is final)."""

    @staticmethod
    def edit(run_id: int | None = None, **params) -> dict:
        """Change parameters of the loaded run live (loads ``run_id`` first).

        Keys: name, chem, cells, capacity_mah, soc, cutoff_mv_cell,
        rint_uohm_cell, peukert, self_discharge, ext_load, dither. Unknown keys
        raise ``TypeError``. Returns :func:`status`.
        """

    @staticmethod
    def delete(run_id: int) -> None:
        """Delete a stored run."""
