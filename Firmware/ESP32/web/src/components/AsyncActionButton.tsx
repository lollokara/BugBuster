import type { ComponentChildren } from "preact";

export function AsyncActionButton({
  busy,
  children,
  busyLabel = "…",
  class: className = "btn primary",
  disabled = false,
  onClick,
}: {
  busy: boolean;
  children: ComponentChildren;
  busyLabel?: ComponentChildren;
  class?: string;
  disabled?: boolean;
  onClick?: () => void | Promise<void>;
}) {
  return (
    <button class={className} onClick={onClick} disabled={disabled || busy}>
      {busy ? busyLabel : children}
    </button>
  );
}
