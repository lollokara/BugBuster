"""BugBuster MCP — Resource registration."""


def register_all(mcp) -> None:
    from .state import register
    register(mcp)
