const toneClass = {
    ok: "text-ok",
    warn: "text-warn",
    err: "text-err",
    info: "text-dim",
};
export function ResultBanner({ tone = "info", children, }) {
    if (!children)
        return null;
    return <div class={toneClass[tone]} style={{ fontSize: "0.8rem" }}>{children}</div>;
}
