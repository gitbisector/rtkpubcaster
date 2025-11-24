# Quick Start - ESP32 RTKPubcaster Monitor

## First Time Setup

```bash
cd tools
./setup.sh
```

## Daily Use

```bash
# 1. Activate venv
cd tools
source venv/bin/activate

# 2. Run monitor
./ably_monitor.py

# 3. Watch the output...
# Press Ctrl+C to stop and see summary

# 4. Deactivate when done
deactivate
```

## What You'll See

### ✅ Base comes online
```
[14:32:15] ✅ BASE ONLINE: esp32_b59d3c
  Position: 37.7749°, -122.4194° @ 10.5m
  GNSS: RTK_FIXED | Satellites: 28
```

### 📡 Real-time messages
```
[14:32:16.123] Seq:42 | 4 msgs | 2048b | 1.0 msg/s | 2.0 KB/s | Types: 1074(512b), 1084(498b)
```

### 📊 Summary on exit
```
Runtime: 120.5 seconds
Messages received: 120
Data received: 245,760 bytes (240.00 KB)
Average message rate: 1.00 msg/s

RTCM3 Message Types Received:
  1074: 120 messages (GPS)
  1084: 120 messages (GLONASS)
  1094: 120 messages (Galileo)
  1124: 120 messages (BeiDou)
```

## Troubleshooting

**No messages?**
- Check ESP32 web interface at http://192.168.50.134/ably.html
- Verify ESP32 shows "Connected" to Ably
- Make sure UM980 GPS is connected

**"Module not found"?**
```bash
cd tools
./setup.sh  # Re-run setup
```

**Wrong channel?**
```bash
ABLY_CHANNEL="base.1.rtk" ./ably_monitor.py
```
