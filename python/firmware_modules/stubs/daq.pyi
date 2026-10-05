"""DAQ HAT control from on-device scripts: the VDUT programmable supply and the
battery simulator.

Every call raises ``OSError(ENODEV)`` when no DAQ HAT is connected (check with
``present()`` first); HAT link failures raise ``OSError(EIO)``; battery-simulator
refusals raise ``RuntimeError`` naming the simulator's reason (for example
``"busy"``). A loaded battery-simulator run owns the VDUT output: ``vdut()``
refuses to change it until the run is unloaded.

Example:
    import daq
    if daq.present():
        daq.vdut(True, volts=3.3, amps_limit=0.5)
        print(daq.read())
        daq.vdut(False)
"""

def present() -> bool:
    """Report whether a DAQ HAT is connected. Never raises.

    Returns:
        True when a DAQ HAT is connected and answering.

    Example:
        if not daq.present():
            raise OSError("connect the DAQ HAT first")
    """

def vdut(enable: bool | None = None, *, volts: float | None = None,
         amps_limit: float | None = None) -> dict:
    """Set the VDUT supply (setpoint, current limit, output) and return its state.

    Pass any of the arguments; with none it only reports. The setpoint is
    applied before ``enable``, so ``vdut(True, volts=3.3)`` never switches on at
    the old voltage. Values outside the range are rejected, never clamped.

    Args:
        enable: True switches the output on, False off, None leaves it as it is.
        volts: Output setpoint in volts, 1.76-19.94.
        amps_limit: Current limit in amps, 0.05-2.636.

    Returns:
        A dict with the supply state after the change.

    Keys:
        present: DAQ HAT present.
        enabled: Output is on.
        fault: Supply reports a fault (for example current limit).
        volts: Programmed setpoint in volts.
        amps_limit: Programmed current limit in amps.
        v: Measured output voltage in volts.
        i: Measured output current in amps.

    Raises:
        ValueError: ``volts`` or ``amps_limit`` is outside its range.
        RuntimeError: a battery-simulator run is loaded and owns VDUT ("unload it first").
        OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

    Safety:
        ``enable=True`` powers the DUT output at the setpoint. Check the setpoint
        and limit first, and switch it off in a ``finally`` block.

    Example:
        state = daq.vdut(True, volts=3.3, amps_limit=0.2)
        print(state["v"], state["i"])
        daq.vdut(False)
    """

def read() -> dict:
    """Measure the DUT output of the VDUT supply.

    Returns:
        A dict with the live measurement.

    Keys:
        v: Output voltage in volts.
        i: Output current in amps.
        p: Power in watts (v * i).
        enabled: Output is on.

    Raises:
        OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

    Example:
        r = daq.read()
        print("%.3f V  %.1f mA" % (r["v"], r["i"] * 1e3))
    """

def samples(run_id: int, since_s: int = 0, max: int = 600) -> list[tuple[int, float, float, float, int]]:
    """Fetch the live 1 s samples of the loaded battery-simulator run.

    Only the loaded run's last hour is available. The call pages through the HAT
    until ``max`` samples are returned or no more are available, and honours the
    Stop button.

    Args:
        run_id: Run id, 1-65535; must be the loaded run.
        since_s: Return samples with a time newer than this many simulated seconds. Default 0.
        max: Maximum number of samples, 1-3600. Default 600.

    Returns:
        A list of ``(t_s, v, i, soc, flags)`` tuples: interval end in simulated
        seconds, mean voltage in volts, mean current in amps, state of charge in
        percent, and history flags (bit mask: 1 gap, 2 current clamp, 4 resume,
        8 output off, 16 below cutoff).

    Raises:
        ValueError: ``run_id`` is outside 1-65535 or ``max`` outside 1-3600.
        OSError: ENOENT when ``run_id`` is not the loaded run; ENODEV; EIO.
        KeyboardInterrupt: the script was stopped.

    Example:
        for t_s, v, i, soc, flags in daq.samples(rid, since_s=0, max=60):
            print(t_s, v, soc)
    """

class run:
    """Battery-simulator runs. A namespace: call as ``daq.run.status()``.

    A run models a battery (chemistry, cells, capacity, state of charge) behind
    the VDUT output. The typical flow is ``new`` (creates and loads a paused run),
    ``start``, poll ``status``/``samples``, then ``stop``. Runs are stored on the
    HAT; ``stop`` keeps the data, ``delete`` removes it.

    Example:
        rid = daq.run.new("demo", "lipo", 1, 500)
        daq.run.start(rid)
        print(daq.run.status()["state"])
        daq.run.stop()
    """

    @staticmethod
    def status() -> dict:
        """Report the loaded run and the simulator state.

        Returns:
            A dict describing the loaded run.

        Keys:
            run_id: Loaded run id, 0 when none.
            state: 'none', 'paused', 'active', 'depleted' or 'stopped'.
            soc: State of charge in percent.
            v: Measured output voltage in volts.
            i: Measured output current in amps.
            elapsed_s: Simulated seconds elapsed.
            remaining_s: Estimated seconds remaining, None when unknown.
            energy_j: Energy delivered to the DUT in joules, None when not available.
            error: Last simulator error as text: 'none', 'no store', 'no run',
                'busy', 'invalid', 'state', 'no PD contract', 'acquisition not
                running', 'I/O error', 'not found' or 'run depleted'.
            name: Run name, None when no run is loaded.
            params: Run parameters (the keys accepted by :func:`edit`), None when no run is loaded.

        Raises:
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.
            RuntimeError: the simulator rejected the request.

        Example:
            st = daq.run.status()
            print(st["state"], "%.1f %%" % st["soc"])
        """

    @staticmethod
    def list() -> list[dict]:
        """List every stored run.

        Returns:
            One dict per run.

        Keys:
            run_id: Run id.
            name: Run name (None if its metadata could not be read).
            created: Creation time as a Unix epoch in seconds.
            chem: Chemistry: 'lipo', 'lifepo4', 'nimh' or 'lead'.
            cells: Number of cells in series.
            capacity_mah: Capacity in milliamp-hours.
            active: True for the loaded run.

        Raises:
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            for r in daq.run.list():
                print(r["run_id"], r["name"])
        """

    @staticmethod
    def new(name: str, chem: str | int, cells: int, capacity_mah: int, soc: float = 100,
            self_discharge: float | None = None, ext_load: int | None = None) -> int:
        """Create a run, load it (paused, output off) and return its id.

        Args:
            name: Run name, at most 23 bytes.
            chem: Chemistry: 'lipo', 'lifepo4', 'nimh' or 'lead' (or the number 0-3 in that order).
            cells: Cells in series.
            capacity_mah: Capacity in milliamp-hours.
            soc: Starting state of charge in percent. Default 100.
            self_discharge: Self-discharge in percent per month; None (default) disables it.
            ext_load: Extra virtual load in microamps; None (default) disables it.

        Returns:
            The new run id.

        Raises:
            ValueError: ``name`` is too long, ``chem`` is unknown or a value is out of range.
            RuntimeError: the simulator rejected the request (for example "busy").
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            rid = daq.run.new("cell A", "lifepo4", 4, 2500, soc=80)
        """

    @staticmethod
    def start(run_id: int | None = None) -> None:
        """Start the run, or resume it from pause.

        Args:
            run_id: Load this run first; None (default) starts the loaded run.

        Raises:
            ValueError: ``run_id`` is outside 1-65535.
            RuntimeError: the simulator refused (the message names the reason).
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Safety:
            Hands the VDUT output to the simulator, which switches it on.

        Example:
            daq.run.start(rid)
        """

    @staticmethod
    def pause() -> None:
        """Switch the output off and freeze the run.

        Raises:
            RuntimeError: the simulator refused.
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            daq.run.pause()
        """

    @staticmethod
    def stop() -> None:
        """Finalise the run; its data stays on the HAT.

        Raises:
            RuntimeError: the simulator refused.
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            daq.run.stop()
        """

    @staticmethod
    def reopen(run_id: int | None = None) -> None:
        """Resume a stopped run: it returns to 'paused' with the output off.

        Call :func:`start` afterwards to continue.

        Args:
            run_id: Load this run first; None (default) uses the loaded run.

        Raises:
            ValueError: ``run_id`` is outside 1-65535.
            RuntimeError: the run is 'depleted' (the cutoff is final) or the simulator refused.
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            daq.run.reopen(rid)
            daq.run.start()
        """

    @staticmethod
    def edit(run_id: int | None = None, **params) -> dict:
        """Change parameters of the loaded run while it is live.

        Args:
            run_id: Load this run first; None (default) edits the loaded run.
            **params: Parameters to change:
                name (str, <= 23 bytes); chem ('lipo' | 'lifepo4' | 'nimh' | 'lead');
                cells (int); capacity_mah (int); soc (percent); cutoff_mv_cell
                (millivolts per cell); rint_uohm_cell (internal resistance in
                micro-ohms per cell); peukert (exponent); self_discharge (percent per
                month, None/0 disables); ext_load (microamps, None/0 disables);
                dither (bool).

        Returns:
            The run status, as :func:`status`.

        Keys:
            run_id: Loaded run id.
            state: 'none', 'paused', 'active', 'depleted' or 'stopped'.
            soc: State of charge in percent.
            v: Measured output voltage in volts.
            i: Measured output current in amps.
            elapsed_s: Simulated seconds elapsed.
            remaining_s: Estimated seconds remaining, None when unknown.
            energy_j: Energy delivered to the DUT in joules, None when not available.
            error: Last simulator error as text.
            name: Run name.
            params: The run parameters after the edit.

        Raises:
            TypeError: an unknown parameter name.
            ValueError: a value is out of range or ``run_id`` is outside 1-65535.
            RuntimeError: the simulator rejected a setting.
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            daq.run.edit(capacity_mah=800, cutoff_mv_cell=3000)
        """

    @staticmethod
    def delete(run_id: int) -> None:
        """Delete a stored run and its data.

        Args:
            run_id: Run id, 1-65535.

        Raises:
            ValueError: ``run_id`` is outside 1-65535.
            RuntimeError: the simulator refused (the message names the reason).
            OSError: ENODEV without a DAQ HAT; EIO when the HAT does not answer.

        Example:
            daq.run.delete(rid)
        """
