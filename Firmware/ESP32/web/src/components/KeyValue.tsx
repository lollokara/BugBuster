import type { ComponentChildren } from "preact";

export function KeyValueRow({
  label,
  children,
}: {
  label: ComponentChildren;
  children: ComponentChildren;
}) {
  return (
    <div class="kv-row">
      <span class="uppercase-tag">{label}</span>
      <span>{children}</span>
    </div>
  );
}

export function KeyValueTable({
  children,
}: {
  children: ComponentChildren;
}) {
  return <table class="kv-table">{children}</table>;
}
