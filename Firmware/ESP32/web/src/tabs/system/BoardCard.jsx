import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import { api, PairingRequiredError, } from "../../api/client";
import { boardState, deviceMac } from "../../state/signals";
export function BoardCard() {
    const [selected, setSelected] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState(null);
    useEffect(() => {
        let alive = true;
        const load = async () => {
            try {
                const r = await api.board();
                if (alive) {
                    boardState.value = r;
                    setSelected(r.active);
                }
            }
            catch (e) {
                if (alive)
                    setErr(e instanceof Error ? e.message : String(e));
            }
        };
        load();
        return () => {
            alive = false;
        };
    }, []);
    const state = boardState.value;
    const available = Array.isArray(state?.available) ? state.available : [];
    const active = available.find((b) => b.id === state.active);
    const apply = async () => {
        const mac = deviceMac.value;
        if (!mac || !selected)
            return;
        setBusy(true);
        setErr(null);
        try {
            await api.boardSelect(mac, selected);
            const r = await api.board();
            boardState.value = r;
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setErr(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setBusy(false);
        }
    };
    return (<GlassCard title="Board Profile">
      {err && <div class="text-err" style={{ fontSize: "0.8rem" }}>{err}</div>}
      <div class="kv-row">
        <span class="uppercase-tag">Active</span>
        <span class="mono">{state?.active ?? "—"}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Select</span>
        <select class="input" value={selected ?? ""} onChange={(e) => setSelected(e.currentTarget.value || null)}>
          <option value="">(none)</option>
          {available.map((b) => (<option key={b.id} value={b.id}>{b.name}</option>))}
        </select>
        <button class="btn primary" onClick={apply} disabled={busy || !selected}>
          {busy ? "…" : "Apply"}
        </button>
      </div>

      {active && (<table class="kv-table">
          <thead>
            <tr>
              <th>Rail</th>
              <th>Value</th>
              <th>Lock</th>
            </tr>
          </thead>
          <tbody>
            {["vlogic", "vadj1", "vadj2"].map((k) => {
                const rail = active.rails[k];
                return (<tr key={k}>
                  <td class="uppercase-tag">{k}</td>
                  <td class="mono">{rail.value.toFixed(3)} V</td>
                  <td>
                    {rail.locked
                        ? <Led state="warn" label="locked"/>
                        : <Led state="off" label="free"/>}
                  </td>
                </tr>);
            })}
          </tbody>
        </table>)}
    </GlassCard>);
}
