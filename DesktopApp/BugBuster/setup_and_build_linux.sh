#!/bin/bash
set -e

echo "=== BugBuster Linux Build Script ==="
echo ""

# --- 1. System dependencies (Tauri v2 requirements for Debian/Ubuntu) ---
echo "[1/5] Installing system dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    curl \
    wget \
    file \
    libssl-dev \
    libgtk-3-dev \
    libayatana-appindicator3-dev \
    librsvg2-dev \
    libwebkit2gtk-4.1-dev \
    libjavascriptcoregtk-4.1-dev \
    libsoup-3.0-dev \
    libudev-dev \
    pkg-config

# --- 2. Rust (skip if already installed) ---
echo ""
echo "[2/5] Checking Rust installation..."
if command -v rustup &> /dev/null; then
    echo "Rust already installed, updating..."
    rustup update stable
else
    echo "Installing Rust..."
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    source "$HOME/.cargo/env"
fi

# --- 3. WASM target ---
echo ""
echo "[3/5] Adding wasm32-unknown-unknown target..."
rustup target add wasm32-unknown-unknown

# --- 4. Cargo tools (trunk + tauri-cli) ---
echo ""
echo "[4/5] Installing trunk and tauri-cli..."
if ! command -v trunk &> /dev/null; then
    cargo install trunk
else
    echo "trunk already installed"
fi

if ! cargo tauri --version &> /dev/null 2>&1; then
    cargo install tauri-cli
else
    echo "tauri-cli already installed"
fi

# --- 5. Build the app ---
echo ""
echo "[5/5] Building BugBuster..."
cd "$(dirname "$0")"
cargo tauri build

echo ""
echo "=== Build complete! ==="
echo "Binary located at: src-tauri/target/release/bugbuster"
echo "Bundle located in: src-tauri/target/release/bundle/"
