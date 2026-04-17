// =============================================================================
// GlassCard — titled panel with optional actions slot.
// =============================================================================

import type { ComponentChildren } from "preact";

export interface GlassCardProps {
  title?: string;
  actions?: ComponentChildren;
  children: ComponentChildren;
  class?: string;
}

export function GlassCard({ title, actions, children, class: extra }: GlassCardProps) {
  const cls = "glass-card" + (extra ? " " + extra : "");
  return (
    <section class={cls}>
      {(title || actions) && (
        <div class="card-header">
          {title && <div class="card-title">{title}</div>}
          {actions && <div class="card-actions">{actions}</div>}
        </div>
      )}
      {children}
    </section>
  );
}
