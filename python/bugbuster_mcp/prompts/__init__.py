"""BugBuster MCP — Prompt template registration."""


def register_all(mcp) -> None:
    from .workflows import register
    register(mcp)
