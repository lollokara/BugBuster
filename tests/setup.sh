#!/usr/bin/env bash
# setup.sh — set up the BugBuster test environment on macOS using Homebrew
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV_DIR="$REPO_ROOT/.venv"

echo "==> BugBuster test environment setup"
echo "    Repo: $REPO_ROOT"
echo "    Venv: $VENV_DIR"
echo ""

# ---------------------------------------------------------------------------
# 1. Ensure Homebrew Python is available
# ---------------------------------------------------------------------------
if ! command -v brew &>/dev/null; then
    echo "ERROR: Homebrew is not installed."
    echo "       Install it from https://brew.sh, then re-run this script."
    exit 1
fi

if ! brew list python3 &>/dev/null 2>&1; then
    echo "==> Installing Python via Homebrew..."
    brew install python3
else
    echo "==> Python already installed via Homebrew"
fi

# Use the Homebrew-managed python3 explicitly
BREW_PYTHON="$(brew --prefix)/bin/python3"
if [[ ! -x "$BREW_PYTHON" ]]; then
    # Fallback: find any python3 from brew
    BREW_PYTHON="$(brew --prefix)/bin/python3.12"
fi
if [[ ! -x "$BREW_PYTHON" ]]; then
    BREW_PYTHON="$(which python3)"
fi

echo "==> Using Python: $BREW_PYTHON ($($BREW_PYTHON --version))"

# ---------------------------------------------------------------------------
# 2. Create virtual environment
# ---------------------------------------------------------------------------
if [[ -d "$VENV_DIR" ]]; then
    echo "==> Virtual environment already exists at $VENV_DIR"
else
    echo "==> Creating virtual environment..."
    "$BREW_PYTHON" -m venv "$VENV_DIR"
fi

# Activate
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
echo "==> Activated: $VIRTUAL_ENV"

# ---------------------------------------------------------------------------
# 3. Upgrade pip inside the venv (never touches system Python)
# ---------------------------------------------------------------------------
echo "==> Upgrading pip..."
pip install --quiet --upgrade pip

# ---------------------------------------------------------------------------
# 4. Install test dependencies
# ---------------------------------------------------------------------------
echo "==> Installing test dependencies..."
pip install --quiet -r "$REPO_ROOT/tests/requirements-test.txt"

# ---------------------------------------------------------------------------
# 5. Install BugBuster Python library in editable mode
# ---------------------------------------------------------------------------
echo "==> Installing BugBuster library (editable)..."
pip install --quiet -e "$REPO_ROOT/python/"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "==> Setup complete!"
echo ""
echo "To activate the environment in a new shell:"
echo "    source .venv/bin/activate"
echo ""
echo "To run tests:"
echo "    python tests/run_tests.py --usb /dev/cu.usbmodem1234"
echo "    python tests/run_tests.py --http 192.168.4.1"
echo "    python tests/run_tests.py --usb /dev/cu.usbmodem1234 --http 192.168.4.1 --hat"
