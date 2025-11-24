# Ably API Setup Instructions

This document explains how to set up Ably API credentials for the ESP32 RTK base station.

## Prerequisites

- Ably account (sign up at https://ably.com)
- Access to Ably dashboard

## Step 1: Create Ably App

1. Log in to Ably dashboard: https://ably.com/dashboard
2. Click "Create New App"
3. Name it: "ESP32 RTK Streaming" (or your preference)
4. Select your region (choose closest to your deployment)

## Step 2: Create API Keys

### Publisher Key (ESP32 Base Station)

1. In your Ably app, go to "API Keys" tab
2. Click "Create New API Key"
3. Name: "ESP32 Publisher"
4. Capabilities:
   ```json
   {
     "base.1.rtk": ["publish", "presence"]
   }
   ```
5. Save and copy the full API key (format: `name.id:secret`)

### Subscriber Key (iOS App - MVP Only)

1. Click "Create New API Key"
2. Name: "iOS Subscriber (MVP)"
3. Capabilities:
   ```json
   {
     "base.1.rtk": ["subscribe", "presence"]
   }
   ```
4. Save and copy the full API key

⚠️ **Warning:** This subscriber key is for MVP testing only. For production iOS apps, implement token authentication (see Archon knowledge base).

## Step 3: Configure ESP32 Credentials

1. Copy the credentials template:
   ```bash
   cp main/ably_credentials.h.example main/ably_credentials.h
   ```

2. Edit `main/ably_credentials.h` with your actual API keys:
   ```c
   #define ABLY_API_KEY_PUBLISHER "your_key_name:your_key_secret"
   #define ABLY_API_KEY_SUBSCRIBER "your_key_name:your_key_secret"
   #define ABLY_CHANNEL_NAME "base.1.rtk"
   ```

3. The file is gitignored and will not be committed

## Step 4: Build and Flash

```bash
idf.py build
idf.py flash monitor
```

## Verification

After flashing, check the serial monitor for:
```
I (xxxx) ably_client: Connecting to mqtt.ably.io:8883
I (xxxx) ably_client: Connected to Ably
I (xxxx) ably_client: Entered presence on channel base.1.rtk
```

## Channel Naming Convention

The default channel is `base.1.rtk`. If you have multiple base stations, use unique IDs:

- `base.1.rtk`
- `base.2.rtk`
- `base.office.rtk`
- etc.

## Security Notes

- ✅ ESP32 API key is safe (you control the hardware)
- ✅ Publish-only capability limits risk
- ⚠️ iOS API key is for testing only
- ❌ NEVER commit `main/ably_credentials.h` to git
- 🔒 For production, implement token authentication

## Troubleshooting

### "Connection refused" or "Connection timeout"

- Check WiFi connectivity on ESP32
- Verify API key format (should include colon: `name:secret`)
- Confirm port 8883 is not blocked by firewall

### "Authentication failed"

- Verify API key is correct (check dashboard)
- Ensure capabilities include "publish" and "presence"
- Try regenerating the API key

### "Channel not found"

- Channel names are case-sensitive
- Verify channel name matches in credentials file (e.g., `base.1.rtk`)
- Ensure API key capabilities match the channel name exactly

## Next Steps

Once basic connection works:

1. Test publishing RTK corrections
2. Test iOS app subscription
3. Verify presence detection (iOS sees base online/offline)
4. Implement channel occupancy optimization (only publish when subscribers present)
5. For production: Implement token authentication (see Archon KB)

## References

- [Ably Documentation](https://ably.com/docs)
- [Ably MQTT Protocol](https://ably.com/docs/protocols/mqtt)
- [Ably Capabilities](https://ably.com/docs/auth/capabilities)
- Archon Knowledge Base: "Ably Authentication Architecture for ESP32 RTK Streaming"
