#!/bin/bash
#
# Setup script for ESP32 RTKPubcaster monitoring tools
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "======================================================================"
echo "ESP32 RTKPubcaster Tools Setup"
echo "======================================================================"
echo

# Check if Python 3 is available
if ! command -v python3 &> /dev/null; then
    echo "ERROR: python3 not found. Please install Python 3.8 or later."
    exit 1
fi

PYTHON_VERSION=$(python3 --version | cut -d' ' -f2)
echo "✓ Found Python $PYTHON_VERSION"

# Create virtual environment if it doesn't exist
if [ ! -d "venv" ]; then
    echo
    echo "Creating Python virtual environment..."
    python3 -m venv venv
    echo "✓ Virtual environment created"
else
    echo "✓ Virtual environment already exists"
fi

# Activate virtual environment
echo
echo "Activating virtual environment..."
source venv/bin/activate

# Upgrade pip
echo
echo "Upgrading pip..."
pip install --upgrade pip --quiet

# Install dependencies
echo
echo "Installing dependencies..."
pip install -r requirements.txt

echo
echo "======================================================================"
echo "Setup complete!"
echo "======================================================================"
echo
echo "To use the monitoring tool:"
echo
echo "  1. Activate the virtual environment:"
echo "     source venv/bin/activate"
echo
echo "  2. Run the monitor:"
echo "     ./ably_monitor.py"
echo
echo "  3. When done, deactivate:"
echo "     deactivate"
echo
echo "======================================================================"
