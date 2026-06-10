export function KeyValueRow({ label, children, }) {
    return (<div class="kv-row">
      <span class="uppercase-tag">{label}</span>
      <span>{children}</span>
    </div>);
}
export function KeyValueTable({ children, }) {
    return <table class="kv-table">{children}</table>;
}
