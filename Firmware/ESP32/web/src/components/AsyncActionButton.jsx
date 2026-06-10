export function AsyncActionButton({ busy, children, busyLabel = "…", class: className = "btn primary", disabled = false, onClick, }) {
    return (<button class={className} onClick={onClick} disabled={disabled || busy}>
      {busy ? busyLabel : children}
    </button>);
}
