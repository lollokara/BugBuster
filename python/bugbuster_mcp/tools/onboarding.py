"""
BugBuster MCP - onboarding tools.

An agent could previously do nothing to get a board onto a network: the Python
library had wifi_scan/wifi_connect and the whole Quick Setup API, but none of it
was exposed as a tool. That blocked every HTTP-only capability, including P4 and
C6 OTA, which are HTTP-only - so an agent could not update a HAT unaided.

Tools: wifi_scan, wifi_connect, quicksetup_list, quicksetup_get,
quicksetup_save, quicksetup_apply, quicksetup_delete
"""

from __future__ import annotations

from .. import session


def register(mcp) -> None:

    @mcp.tool()
    def wifi_scan() -> dict:
        """
        Scan for nearby WiFi networks.

        Read-only. Takes a few seconds while the radio sweeps channels; the
        device's own SoftAP stays up throughout.

        Returns: networks, a list of {ssid, rssi (dBm, higher is better), auth}.
        """
        bb = session.get_client()
        networks = bb.wifi_scan()
        return {"networks": networks, "count": len(networks)}

    @mcp.tool()
    def wifi_connect(ssid: str, password: str) -> dict:
        """
        Join a WiFi network so HTTP-only features become reachable.

        Needed before anything HTTP-only works - P4 and C6 firmware upload in
        particular have no USB route at all. Credentials persist to NVS, so the
        device rejoins on its own after a reboot.

        This does NOT disturb the USB control link, and the SoftAP stays up, so
        it is safe to call while connected over USB.

        Parameters:
        - ssid: Network name. Case-sensitive.
        - password: WPA2 passphrase. Pass an empty string for an open network.

        Returns: success, ssid, and the resulting status (ip, rssi, connected).
        Association can take a few seconds; if connected is False, re-read
        wifi_status before concluding it failed.
        """
        bb = session.get_client()
        ok = bool(bb.wifi_connect(ssid, password))
        status = {}
        try:
            status = bb.wifi_get_status()
        except Exception as exc:  # noqa: BLE001 - status is advisory here
            status = {"error": str(exc)}
        return {"success": ok, "ssid": ssid, "status": status}

    @mcp.tool()
    def quicksetup_list() -> dict:
        """
        List the saved Quick Setup slots.

        A Quick Setup slot is a stored snapshot of channel functions, MUX
        routing and rail setpoints that can be re-applied in one step.

        Read-only.

        Returns: slots, a list of {slot, name, occupied}.
        """
        bb = session.get_client()
        slots = bb.quicksetup_list()
        return {"slots": slots, "count": len(slots)}

    @mcp.tool()
    def quicksetup_get(slot: int) -> dict:
        """
        Read the contents of one Quick Setup slot.

        Read-only.

        Parameters:
        - slot: Slot index, as reported by quicksetup_list.

        Returns: the stored configuration, or an empty result if the slot is free.
        """
        bb = session.get_client()
        return {"slot": slot, "config": bb.quicksetup_get(slot)}

    @mcp.tool()
    def quicksetup_save(slot: int, i_understand_the_risk: bool = False) -> dict:
        """
        OVERWRITES the target slot in device NVS with the CURRENT hardware state.

        The previous contents of the slot are lost and cannot be recovered.
        You MUST pass i_understand_the_risk=True.

        Read the slot with quicksetup_get first if you might need its contents.

        Parameters:
        - slot: Slot index to overwrite.
        - i_understand_the_risk: Must be True.

        Returns: the save result reported by the device.
        """
        if not i_understand_the_risk:
            raise ValueError(
                "quicksetup_save overwrites the slot's stored configuration in NVS "
                "and the old contents cannot be recovered. Read it with "
                "quicksetup_get first, then pass i_understand_the_risk=True."
            )
        bb = session.get_client()
        return bb.quicksetup_save(slot)

    @mcp.tool()
    def quicksetup_apply(slot: int, i_understand_the_risk: bool = False) -> dict:
        """
        DRIVES THE HARDWARE: reconfigures channels, MUX routing and rails at once.

        This changes physical outputs. If a DUT is attached, it will see the new
        channel functions and rail voltages immediately. You MUST pass
        i_understand_the_risk=True.

        Inspect the slot with quicksetup_get before applying it.

        Parameters:
        - slot: Slot index to apply.
        - i_understand_the_risk: Must be True.

        Returns: the apply result reported by the device.
        """
        if not i_understand_the_risk:
            raise ValueError(
                "quicksetup_apply reconfigures channel functions, MUX routing and "
                "rail voltages in one step, which an attached DUT will see "
                "immediately. Inspect the slot with quicksetup_get, then pass "
                "i_understand_the_risk=True."
            )
        bb = session.get_client()
        return bb.quicksetup_apply(slot)

    @mcp.tool()
    def quicksetup_delete(slot: int, i_understand_the_risk: bool = False) -> dict:
        """
        PERMANENTLY ERASES a Quick Setup slot from device NVS.

        There is no undo. You MUST pass i_understand_the_risk=True.

        Parameters:
        - slot: Slot index to erase.
        - i_understand_the_risk: Must be True.

        Returns: the delete result reported by the device.
        """
        if not i_understand_the_risk:
            raise ValueError(
                "quicksetup_delete erases the slot from NVS permanently. Read it "
                "with quicksetup_get first if you may need it, then pass "
                "i_understand_the_risk=True."
            )
        bb = session.get_client()
        return bb.quicksetup_delete(slot)
