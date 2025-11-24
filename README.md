# ESP32 RTKPubcaster

ESP32 firmware for streaming RTCM3 corrections from a UM980 GPS receiver to mobile clients via Ably MQTT.

Forked from [esp32-ntrip-DUO](https://github.com/incarvr6/esp32-ntrip-DUO), redesigned for RTK base station operation with real-time correction streaming.

## Hardware

- **ESP32-C6** (primary target)
- **UM980 GPS receiver** (dual-mode RTK capable, 50Hz update rate)
- UART communication (default: TX gpio1, RX gpio3)

## Features

- RTCM3 message batching and streaming
- Ably MQTT integration with real-time occupancy tracking
- Conditional publishing (only when subscribers present)
- Web-based configuration interface
- OTA firmware updates with signature verification and rollback
- Configurable RTCM message rates (0.02s to 60s)
- GPS status monitoring and NMEA parsing

## Build

Requires ESP-IDF 5.5+:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Configuration

1. Copy `main/ably_credentials.h.example` to `main/ably_credentials.h`
2. Add your Ably API keys and channel name
3. Configure via web interface at `http://192.168.4.1` (AP mode) or device IP

## Architecture

```
UM980 GPS → UART → ESP32 → Ably MQTT → Mobile Clients
  (RTCM3)           (Batch)   (TLS)     (Subscribe)
```

See `ABLY_SETUP.md` for Ably channel configuration details.

## License

GPLv3 - See LICENSE file.

Originally derived from [esp32-xbee](https://github.com/nebkat/esp32-xbee) by Nebojsa Cvetkovic, forked as [esp32-ntrip-DUO](https://github.com/incarvr6/esp32-ntrip-DUO), and redesigned as RTKPubcaster for Ably-based RTK correction streaming.
