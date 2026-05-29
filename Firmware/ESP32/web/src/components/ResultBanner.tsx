import type { ComponentChildren } from "preact";

type Tone = "ok" | "warn" | "err" | "info";

const toneClass: Record<Tone, string> = {
  ok: "text-ok",
  warn: "text-warn",
  err: "text-err",
  info: "text-dim",
};

export function ResultBanner({
  tone = "info",
  children,
}: {
  tone?: Tone;
  children: ComponentChildren;
}) {
  if (!children) return null;
  return <div class={toneClass[tone]} style={{ fontSize: "0.8rem" }}>{children}</div>;
}
