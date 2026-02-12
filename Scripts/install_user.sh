#!/bin/bash
# Post-install script for tools not available in pacman/AUR
# Run this after the main HyDE install completes

set -e

echo "[*] Installing user tools..."

# --------------------------------------------------- // uv (Python package manager)
if ! command -v uv &>/dev/null; then
    echo "[+] Installing uv (astral.sh)..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
else
    echo "[=] uv already installed"
fi

# --------------------------------------------------- // Cursor (AI code editor)
if ! command -v cursor &>/dev/null; then
    echo "[+] Installing Cursor..."
    curl -fsS https://cursor.com/install | bash
else
    echo "[=] Cursor already installed"
fi

# --------------------------------------------------- // phpv (PHP version manager)
if ! command -v phpv &>/dev/null; then
    if command -v go &>/dev/null; then
        echo "[+] Installing phpv..."
        go install github.com/Its-Satyajit/phpv@latest
    else
        echo "[!] Go not installed, skipping phpv"
    fi
else
    echo "[=] phpv already installed"
fi

echo "[*] User tools installation complete!"
