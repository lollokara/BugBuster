import { GlassCard } from "../../components/GlassCard";
import { usePoll } from "../../hooks/usePoll";
import { api } from "../../api/client";
export function DebugCard() {
    const dbg = usePoll(() => api.debug(), 2000);
    return (<GlassCard title="Debug (raw)">
      <pre class="debug-dump mono">
        {JSON.stringify(dbg, null, 2)}
      </pre>
    </GlassCard>);
}
