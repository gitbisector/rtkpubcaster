#!/bin/bash
# ESP32 OTA Deployment Script
# Usage: ./deploy_ota.sh [device_ip] [username] [password]

DEVICE_IP="${1:-192.168.50.134}"
USERNAME="${2:-admin}"
PASSWORD="${3:-admin}"
FIRMWARE="build/esp32-rtkpubcaster.bin"

# Check firmware exists
if [ ! -f "$FIRMWARE" ]; then
    echo "❌ Error: Firmware not found at $FIRMWARE"
    echo "Run 'idf.py build' first"
    exit 1
fi

# Get firmware size
SIZE=$(stat -f%z "$FIRMWARE" 2>/dev/null || stat -c%s "$FIRMWARE")
SIZE_MB=$(echo "scale=2; $SIZE/1024/1024" | bc)

echo "═══════════════════════════════════════════════"
echo "  ESP32 OTA Firmware Deployment"
echo "═══════════════════════════════════════════════"
echo "📦 Firmware: $FIRMWARE ($SIZE_MB MB)"
echo "🎯 Target: $DEVICE_IP"
echo "🔐 Auth: $USERNAME:****"
echo ""
echo "Uploading firmware..."
echo ""

# Upload with authentication
curl -X POST \
  -u "$USERNAME:$PASSWORD" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@$FIRMWARE" \
  -w "\n📊 HTTP Status: %{http_code}\n" \
  -w "⏱️  Upload time: %{time_total}s\n" \
  -w "📈 Upload speed: %{speed_upload} bytes/sec\n" \
  "http://$DEVICE_IP/api/ota/upload"

echo ""
echo "═══════════════════════════════════════════════"
echo "✅ OTA upload complete"
echo "⏳ Device will restart automatically..."
echo "═══════════════════════════════════════════════"
