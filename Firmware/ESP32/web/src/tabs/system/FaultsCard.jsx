import { useEffect, useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac } from "../../state/signals";
import { maskToHex, parseMask } from "./systemUtils";
function FaultsCard() {
    const mac = deviceMac.value;
    const faults = useInterval(() => api.faults(), 2000);
    const [alertMask, setAlertMask] = useState("0xFFFF");
    const [supplyMask, setSupplyMask] = useState("0xFFFF");
    const [channelMask, setChannelMask] = useState({});
    const [masksDirty, setMasksDirty] = useState(false);
    useEffect(() => {
        if (masksDirty)
            return;
        const am = Number(faults?.alertMask ?? faults?.alert_mask ?? 0);
        const sm = Number(faults?.supplyAlertMask ?? faults?.supply_alert_mask ?? 0);
        setAlertMask(maskToHex(am));
        setSupplyMask(maskToHex(sm));
        const next = {};
        const channels = Array.isArray(faults?.channels) ? faults.channels : [];
        for (const ch of channels) {
            const id = Number(ch?.id ?? 0);
            const mask = Number(ch?.channelAlertMask ?? ch?.mask ?? 0);
            next[id] = maskToHex(mask);
        }
        setChannelMask(next);
    }, [faults, masksDirty]);
    const clearAll = async () => {
        if (!mac)
            return;
        try {
            await api.faultsClearAll(mac);
            setMasksDirty(false);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError))
                console.warn("faultsClearAll failed", e);
        }
    };
    const clearChannel = async (id) => {
        if (!mac)
            return;
        try {
            await api.faultsClearChannel(mac, id);
            setMasksDirty(false);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                console.warn("faultsClearChannel failed", e);
            }
        }
    };
    const applyGlobalMasks = async () => {
        if (!mac)
            return;
        try {
            await api.faultsSetMasks(mac, parseMask(alertMask), parseMask(supplyMask));
            setMasksDirty(false);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError))
                console.warn("faultsSetMasks failed", e);
        }
    };
    const applyChannelMask = async (id) => {
        if (!mac)
            return;
        try {
            await api.faultsSetChannelMask(mac, id, parseMask(channelMask[id] ?? "0"));
            setMasksDirty(false);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                console.warn("faultsSetChannelMask failed", e);
            }
        }
    };
    const alertStatus = Number(faults?.alertStatus ?? faults?.alert_status ?? 0);
    const supplyStatus = Number(faults?.supplyAlertStatus ?? faults?.supply_alert_status ?? 0);
    const channels = Array.isArray(faults?.channels) ? faults.channels : [];
    return (<GlassCard title="Faults" actions={<button class="btn" disabled={!mac} onClick={clearAll}>Clear all</button>}>
      <div class="kv-row">
        <span class="uppercase-tag">Alert Status</span>
        <span class="mono">{maskToHex(alertStatus)}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Supply Status</span>
        <span class="mono">{maskToHex(supplyStatus)}</span>
      </div>

      <details>
        <summary class="uppercase-tag">Global Masks</summary>
        <div class="analog-row">
          <label>Alert mask</label>
          <input class="input" value={alertMask} onInput={(e) => { setMasksDirty(true); setAlertMask(e.currentTarget.value); }}/>
        </div>
        <div class="analog-row">
          <label>Supply mask</label>
          <input class="input" value={supplyMask} onInput={(e) => { setMasksDirty(true); setSupplyMask(e.currentTarget.value); }}/>
        </div>
        <button class="btn" disabled={!mac} onClick={applyGlobalMasks}>Apply masks</button>
      </details>

      <table class="kv-table">
        <thead>
          <tr><th>CH</th><th>Status</th><th>Mask</th><th></th><th></th></tr>
        </thead>
        <tbody>
          {channels.map((ch) => {
            const id = Number(ch?.id ?? 0);
            const status = Number(ch?.channelAlert ?? ch?.alert ?? 0);
            return (<tr key={id}>
                <td class="mono">{id}</td>
                <td class="mono">{maskToHex(status)}</td>
                <td>
                  <input class="input" value={channelMask[id] ?? "0x0000"} onInput={(e) => {
                    setMasksDirty(true);
                    setChannelMask({
                        ...channelMask,
                        [id]: e.currentTarget.value,
                    });
                }}/>
                </td>
                <td>
                  <button class="pill" disabled={!mac} onClick={() => applyChannelMask(id)}>Apply</button>
                </td>
                <td>
                  <button class="pill" disabled={!mac} onClick={() => clearChannel(id)}>Clear</button>
                </td>
              </tr>);
        })}
          {channels.length === 0 && (<tr><td colSpan={5} class="text-dim">No channel fault data</td></tr>)}
        </tbody>
      </table>
    </GlassCard>);
}
export { FaultsCard };
