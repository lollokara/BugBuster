#!/usr/bin/env bash
# setup.sh — set up the BugBuster test environment
# Works on macOS (Homebrew) and Linux.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV_DIR="$REPO_ROOT/.venv"

echo "==> BugBuster test environment setup"
echo "    Repo: $REPO_ROOT"
echo "    Venv: $VENV_DIR"
echo ""

# ---------------------------------------------------------------------------
# 1. Locate a Python 3.11+ interpreter
# ---------------------------------------------------------------------------
PYTHON=""
for candidate in python3.13 python3.12 python3.11 python3; do
    if command -v "$candidate" &>/dev/null; then
        ver=$("$candidate" -c "import sys; print(sys.version_info >= (3,11))" 2>/dev/null || echo "False")
        if [[ "$ver" == "True" ]]; then
            PYTHON="$candidate"
            break
        fi
    fi
done

if [[ -z "$PYTHON" ]]; then
    echo "ERROR: Python 3.11 or newer is required."
    echo "       On macOS:  brew install python3"
    echo "       On Ubuntu: sudo apt install python3.11"
    exit 1
fi

echo "==> Using Python: $PYTHON ($($PYTHON --version))"

# ---------------------------------------------------------------------------
# 2. Create virtual environment
# ---------------------------------------------------------------------------
if [[ -d "$VENV_DIR" ]]; then
    echo "==> Virtual environment already exists at $VENV_DIR"
else
    echo "==> Creating virtual environment..."
    "$PYTHON" -m venv "$VENV_DIR"
fi

# Activate
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
echo "==> Activated: $VIRTUAL_ENV"

# ---------------------------------------------------------------------------
# 3. Upgrade pip inside the venv
# ---------------------------------------------------------------------------
echo "==> Upgrading pip..."
pip install --quiet --upgrade pip

# ---------------------------------------------------------------------------
# 4. Install the BugBuster Python library in editable mode (with MCP extra)
# ---------------------------------------------------------------------------
echo "==> Installing BugBuster library (editable, with mcp extra)..."
pip install --quiet -e "$REPO_ROOT/python/[mcp]"

# ---------------------------------------------------------------------------
# 5. Install test dependencies
# ---------------------------------------------------------------------------
echo "==> Installing test dependencies..."
pip install --quiet -r "$REPO_ROOT/tests/requirements-test.txt"

# ---------------------------------------------------------------------------
# 6. Optional: build ESP32 web bundle if pnpm is available
# ---------------------------------------------------------------------------
WEB_DIR="$REPO_ROOT/Firmware/ESP32/web"
if command -v pnpm &>/dev/null && [[ -f "$WEB_DIR/package.json" ]]; then
    echo "==> Building ESP32 web bundle (pnpm)..."
    (cd "$WEB_DIR" && pnpm install --silent && pnpm build --silent) && echo "    web bundle OK" || echo "    web bundle FAILED (non-fatal)"
else
    echo "==> pnpm not found — skipping ESP32 web bundle build"
fi

# ---------------------------------------------------------------------------
# 7. Version summary
# ---------------------------------------------------------------------------
echo ""
echo "==> Installed versions:"
echo "    Python:  $($PYTHON --version)"
echo "    pip:     $(pip --version | awk '{print $2}')"
echo "    pytest:  $(python -m pytest --version 2>&1 | head -1)"
python -c "import bugbuster; print(f'    bugbuster: {bugbuster.__version__}')" || echo "    bugbuster: import failed"
command -v pio   &>/dev/null && echo "    pio:     $(pio --version 2>&1 | head -1)" || echo "    pio:     not found"
command -v cargo &>/dev/null && echo "    cargo:   $(cargo --version 2>&1)"          || echo "    cargo:   not found"
echo ""

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo "==> Setup complete!"
echo ""
echo "To activate the environment in a new shell:"
echo "    source .venv/bin/activate"
echo ""
echo "To run tests (no hardware required):"
echo "    PYTHONPATH=python pytest tests/unit -q"
echo "    PYTHONPATH=python pytest tests/device --sim -q"
echo ""
echo "To run tests with hardware:"
echo "    python tests/run_tests.py --usb /dev/cu.usbmodem1234"
echo "    python tests/run_tests.py --http 192.168.4.1"
echo "    python tests/run_tests.py --usb /dev/cu.usbmodem1234 --http 192.168.4.1 --hat"
