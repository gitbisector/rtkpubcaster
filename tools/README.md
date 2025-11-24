# ESP32 RTKPubcaster Tools

Development and monitoring tools for the ESP32 RTKPubcaster RTK base station.

## Ably Channel Monitor

`ably_monitor.py` - Real-time monitor for RTK correction data streaming through Ably.

### Features

- ✅ Real-time message reception with statistics
- ✅ Base station presence tracking (online/offline status)
- ✅ RTCM3 message type analysis
- ✅ Data rate and bandwidth monitoring
- ✅ Presence data display (position, GNSS status, firmware info)
- ✅ Session summary statistics

### Installation

**Automated setup (recommended):**

```bash
cd tools
./setup.sh
```

This will create a Python virtual environment and install all dependencies.

**Manual setup:**

```bash
cd tools
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

### Usage

**Activate virtual environment first:**

```bash
cd tools
source venv/bin/activate
```

**Basic usage** (reads credentials from `main/ably_credentials.h`):

```bash
./ably_monitor.py
```

**Override channel name:**

```bash
ABLY_CHANNEL="base.1.rtk" ./ably_monitor.py
```

**Override API key:**

```bash
ABLY_API_KEY="your_key_here" ./ably_monitor.py
```

**When done:**

```bash
deactivate  # Exit virtual environment
```

### Example Output

```
======================================================================
ABLY RTK CHANNEL MONITOR
======================================================================
Reading credentials from main/ably_credentials.h...
API Key: Lx86Tw.WHNsEw:CvH...cp_g
Channel: rtk:corrections:base1

Connecting to Ably...
Channel: rtk:corrections:base1
Waiting for base presence and messages...
Press Ctrl+C to stop

[14:32:15] ✅ BASE ONLINE: esp32_b59d3c
  Position: 37.7749°, -122.4194° @ 10.5m
  Accuracy: survey_in
  GNSS: RTK_FIXED | Satellites: 28
  Constellations: GPS, GLONASS, Galileo, BeiDou
  Status: online
  Uptime: 0h 5m
  Firmware: v1.0.0 (build 20241123)

[14:32:16.123] Seq:42 | 4 msgs | 2048b | 1.0 msg/s | 2.0 KB/s | Types: 1074(512b), 1084(498b), 1094(523b), 1124(515b)
[14:32:17.125] Seq:43 | 4 msgs | 2051b | 1.0 msg/s | 2.0 KB/s | Types: 1074(514b), 1084(501b), 1094(520b), 1124(516b)
...

^C
Stopping monitor...

======================================================================
SESSION SUMMARY
======================================================================
Runtime: 120.5 seconds
Messages received: 120
Data received: 245,760 bytes (240.00 KB)
Average message rate: 1.00 msg/s
Average data rate: 1.99 KB/s

RTCM3 Message Types Received:
  1074: 120 messages
  1084: 120 messages
  1094: 120 messages
  1124: 120 messages

Active Base Stations:
  - esp32_b59d3c
======================================================================
Disconnected.
```

### Message Format

The monitor decodes RTK correction messages with this structure:

```json
{
  "type": "rtk_corrections",
  "version": 1,
  "base_id": "base1",
  "timestamp": 1700000000123456,
  "sequence": 12345,
  "batch": {
    "count": 4,
    "total_size": 2048,
    "messages": [
      {
        "type": 1074,
        "size": 512,
        "data": "0xD3...base64..."
      }
    ]
  }
}
```

### RTCM3 Message Types

Common message types you'll see:

- **1005/1006**: Base station position (every 10-30 seconds)
- **1074**: GPS MSM4 observables (1Hz)
- **1084**: GLONASS MSM4 observables (1Hz)
- **1094**: Galileo MSM4 observables (1Hz)
- **1124**: BeiDou MSM4 observables (1Hz)

### Troubleshooting

**"ERROR: Ably Python SDK not found"**
```bash
pip3 install ably
```

**"ERROR: Could not find ABLY_API_KEY_SUBSCRIBER"**

Ensure `main/ably_credentials.h` exists and contains:
```c
#define ABLY_API_KEY_SUBSCRIBER "your_key_here"
```

Or set environment variable:
```bash
export ABLY_API_KEY="your_key_here"
```

**No messages received**

1. Check ESP32 is connected to WiFi
2. Check ESP32 logs show "Connected to Ably MQTT broker"
3. Verify channel name matches between ESP32 and monitor
4. Check UM980 GPS is connected and outputting RTCM3 data

**Connection timeout**

Check your internet connection and firewall settings. Ably uses:
- REST API: `https://rest.ably.io`
- Realtime: `wss://realtime.ably.io`
