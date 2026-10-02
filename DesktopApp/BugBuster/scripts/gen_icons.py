"""Regenerate src/components/icons.rs from lucide-static.
Usage: npm pack lucide-static; tar -xzf lucide-static-*.tgz; python scripts/gen_icons.py package/icons"""
import pathlib, re, sys
ICONS = """layout-dashboard circuit-board gauge triangle-alert stethoscope chart-line arrow-up-from-line
arrow-right-from-line arrow-down-to-line toggle-right log-in log-out zap layers activity binary chart-spline
waves route cpu usb terminal search sun moon monitor panel-left panel-right command download upload
refresh-cw power play square pause settings sliders-horizontal chevron-down chevron-right chevron-left
chevron-up x check circle-check circle-alert circle-x info thermometer wifi plug plug-zap unplug cable
radio memory-stick microchip battery-charging bug lock rotate-ccw file-down file-up external-link
crosshair ruler trash-2 plus minus copy save folder-open list ellipsis funnel timer clock signal
radar split network scan-search circle-dot circle hard-drive cloud-download sparkle target
arrow-left-right move-horizontal zoom-in zoom-out maximize-2 minimize-2 eye eye-off flag bell
house shield-check shield-alert link unlink fingerprint wand-sparkles""".split()
src = pathlib.Path(sys.argv[1])
out = []
missing = []
for name in ICONS:
    p = src / f"{name}.svg"
    if not p.exists():
        missing.append(name)
        continue
    t = p.read_text(encoding="utf-8")
    body = t[t.index(">", t.index("<svg")) + 1 : t.rindex("</svg>")]
    body = re.sub(r"\s*\n\s*", "", body).replace(" />", "/>").strip()
    out.append((name, body))
print("missing:", missing)
rs = ["// Lucide icons (https://lucide.dev), ISC License, Copyright (c) Lucide Contributors.",
      "// Generated from lucide-static; add names to the generator rather than hand-editing paths.",
      "use leptos::prelude::*;", "",
      "pub fn icon_body(name: &str) -> &'static str {", "    match name {"]
for n, b in out:
    rs.append(f'        "{n}" => r#"{b}"#,')
rs += ["        _ => \"\",", "    }", "}", "",
       "/// Inline SVG icon; inherits `currentColor`. Decorative unless `label` is set.",
       "#[component]",
       "pub fn Icon(",
       "    name: &'static str,",
       "    #[prop(default = 16)] size: u32,",
       "    #[prop(optional)] label: Option<&'static str>,",
       "    #[prop(optional)] class: &'static str,",
       ") -> impl IntoView {",
       "    view! {",
       "        <svg",
       "            class=format!(\"icon icon-{name} {class}\")",
       "            width=size",
       "            height=size",
       "            viewBox=\"0 0 24 24\"",
       "            fill=\"none\"",
       "            stroke=\"currentColor\"",
       "            stroke-width=\"1.75\"",
       "            stroke-linecap=\"round\"",
       "            stroke-linejoin=\"round\"",
       "            role=if label.is_some() { \"img\" } else { \"presentation\" }",
       "            aria-hidden=if label.is_some() { \"false\" } else { \"true\" }",
       "            aria-label=label",
       "            inner_html=icon_body(name)",
       "        ></svg>",
       "    }",
       "}", ""]
dst = pathlib.Path(__file__).resolve().parent.parent / "src" / "components" / "icons.rs"
dst.write_text("\n".join(rs), encoding="utf-8", newline="\n")
print("wrote", len(out), "icons")
