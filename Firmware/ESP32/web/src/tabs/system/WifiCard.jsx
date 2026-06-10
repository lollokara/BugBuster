import { useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac } from "../../state/signals";
export function WifiCard() {
    const mac = deviceMac.value;
    const [nets, setNets] = useState([]);
    const [scanning, setScanning] = useState(false);
    const [connecting, setConnecting] = useState(false);
    const [ssid, setSsid] = useState("");
    const [pass, setPass] = useState("");
    const [status, setStatus] = useState(null);
    const scan = async () => {
        setScanning(true);
        setStatus(null);
        try {
            const r = await api.wifiScan();
            setNets(Array.isArray(r?.networks) ? r.networks : Array.isArray(r) ? r : []);
            if (!Array.isArray(r?.networks) && !Array.isArray(r)) {
                setStatus("No scan results");
            }
        }
        catch (e) {
            setStatus(e instanceof Error ? e.message : String(e));
        }
        finally {
            setScanning(false);
        }
    };
    const connect = async () => {
        if (!mac || !ssid)
            return;
        setConnecting(true);
        setStatus(null);
        try {
            const r = await api.wifiConnect(mac, ssid, pass);
            if (r?.success) {
                setStatus(`Connected${r.ip ? ` (${r.ip})` : ""}`);
            }
            else {
                setStatus("Connection failed");
            }
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatus(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setConnecting(false);
        }
    };
    return (<GlassCard title="WiFi">
      <div class="kv-row">
        <button class="btn" onClick={scan} disabled={scanning}>
          {scanning ? "Scanning…" : "Scan"}
        </button>
      </div>
      {status && <div class="text-dim">{status}</div>}
      <ul class="wifi-list">
        {nets.map((n, i) => (<li key={i} onClick={() => setSsid(n.ssid ?? "")}>
            <span class="mono">{n.ssid ?? "—"}</span>
            <span class="text-dim">{n.rssi ?? ""} dBm</span>
          </li>))}
        {nets.length === 0 && <li class="text-dim">No networks yet</li>}
      </ul>
      <div class="kv-row">
        <input class="input" placeholder="SSID" value={ssid} onInput={(e) => setSsid(e.currentTarget.value)}/>
        <input class="input" type="password" placeholder="password" value={pass} onInput={(e) => setPass(e.currentTarget.value)}/>
        <button class="btn primary" onClick={connect} disabled={!mac || !ssid || connecting}>
          {connecting ? "Connecting…" : "Connect"}
        </button>
      </div>
    </GlassCard>);
}
