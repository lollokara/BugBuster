// =============================================================================
// ErrorBoundary — WEB-8: catch tab render errors so one tab crash doesn't take down the whole shell.
// =============================================================================

import { Component, type ComponentChildren } from "preact";

interface ErrorBoundaryProps {
  children: ComponentChildren;
  fallback?: (error: Error, errorInfo?: any) => ComponentChildren;
}

interface ErrorBoundaryState {
  hasError: boolean;
  error: Error | null;
  errorInfo: any;
}

export class ErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
  constructor(props: ErrorBoundaryProps) {
    super(props);
    this.state = { hasError: false, error: null, errorInfo: null };
  }

  static override getDerivedStateFromError(error: Error): Partial<ErrorBoundaryState> {
    return { hasError: true, error };
  }

  override componentDidCatch(error: Error, errorInfo: any) {
    console.error("ErrorBoundary caught:", error, errorInfo);
    this.setState({ error, errorInfo });
  }

  override render() {
    if (this.state.hasError) {
      if (this.props.fallback) {
        return this.props.fallback(this.state.error!, this.state.errorInfo);
      }
      return (
        <div style="padding: 2rem; color: #ef4444;">
          <h3>⚠ Tab Error</h3>
          <p>This tab encountered an error. Try refreshing the page.</p>
          <details style="margin-top: 1rem; font-size: 0.875rem; font-family: monospace;">
            <summary>Error details</summary>
            <pre style="margin-top: 0.5rem; padding: 1rem; background: rgba(0,0,0,0.2); border-radius: 4px; overflow-x: auto;">
              {this.state.error?.toString() ?? "Unknown error"}
              {"\n\n"}
              {this.state.errorInfo?.componentStack ?? ""}
            </pre>
          </details>
        </div>
      );
    }
    return this.props.children;
  }
}
