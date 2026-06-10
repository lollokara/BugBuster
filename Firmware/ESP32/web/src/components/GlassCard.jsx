// =============================================================================
// GlassCard — titled panel with optional actions slot.
// =============================================================================
export function GlassCard({ title, actions, children, class: extra }) {
    const cls = "glass-card" + (extra ? " " + extra : "");
    return (<section class={cls}>
      {(title || actions) && (<div class="card-header">
          {title && <div class="card-title">{title}</div>}
          {actions && <div class="card-actions">{actions}</div>}
        </div>)}
      {children}
    </section>);
}
