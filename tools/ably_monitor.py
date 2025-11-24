#!/usr/bin/env python3
"""
RTKPubcaster Channel Monitor

Subscribes to the RTK corrections channel using the subscriber API key
and displays real-time status information about messages.

Usage:
    python3 ably_monitor.py

Environment Variables:
    ABLY_API_KEY - Override API key from credentials file
    ABLY_CHANNEL - Override channel name (default: rtk:corrections:base1)
"""

import os
import sys
import json
import time
import asyncio
from datetime import datetime
from collections import deque

try:
    from ably import AblyRealtime
except ImportError:
    print("ERROR: Ably Python SDK not found.")
    print("Install with: pip3 install ably")
    sys.exit(1)


class RTKMonitor:
    def __init__(self, api_key, channel_name):
        self.api_key = api_key
        self.channel_name = channel_name

        # Statistics
        self.messages_received = 0
        self.bytes_received = 0
        self.start_time = time.time()
        self.last_message_time = None
        self.message_times = deque(maxlen=60)

        # RTCM3 statistics
        self.rtcm_types_seen = {}

        # Ably client
        self.client = None
        self.channel = None
        self.running = True

    def on_message(self, message):
        """Handle incoming messages"""
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.messages_received += 1
        self.last_message_time = time.time()
        self.message_times.append(self.last_message_time)

        # Try to parse as JSON
        try:
            if isinstance(message.data, str):
                data = json.loads(message.data)
            elif isinstance(message.data, dict):
                data = message.data
            elif isinstance(message.data, (bytes, bytearray)):
                # Decode bytes/bytearray to string then parse JSON
                data = json.loads(message.data.decode('utf-8'))
            else:
                print(f"[{timestamp}] Unknown data type: {type(message.data)}")
                return

            msg_type = data.get('type', 'unknown')

            if msg_type == 'rtk_corrections':
                self.handle_correction_message(timestamp, data)
            else:
                print(f"[{timestamp}] Message type: {msg_type}")

        except (json.JSONDecodeError, AttributeError) as e:
            print(f"[{timestamp}] Parse error: {e}")

    def handle_correction_message(self, timestamp, data):
        """Handle RTK correction messages"""
        sequence = data.get('sequence', 'N/A')
        base_id = data.get('base_id', 'unknown')
        batch = data.get('batch', {})
        count = batch.get('count', 0)
        total_size = batch.get('total_size', 0)

        self.bytes_received += total_size

        # Collect RTCM3 message types
        messages = batch.get('messages', [])
        types_in_batch = []
        for msg in messages:
            msg_type = msg.get('type', 0)
            msg_size = msg.get('size', 0)
            types_in_batch.append(f"{msg_type}({msg_size}b)")

            # Track statistics
            if msg_type not in self.rtcm_types_seen:
                self.rtcm_types_seen[msg_type] = 0
            self.rtcm_types_seen[msg_type] += 1

        # Calculate message rate
        msg_rate = self.calculate_message_rate()
        data_rate = self.calculate_data_rate()

        # Print concise status line
        print(f"[{timestamp}] Seq:{sequence} | {count} msgs | {total_size}b | {msg_rate:.1f} msg/s | {data_rate:.1f} KB/s | Types: {', '.join(types_in_batch)}")

    def calculate_message_rate(self):
        """Calculate messages per second"""
        if len(self.message_times) < 2:
            return 0.0

        time_span = self.message_times[-1] - self.message_times[0]
        if time_span < 0.1:
            return 0.0

        return len(self.message_times) / time_span

    def calculate_data_rate(self):
        """Calculate data rate in KB/s"""
        elapsed = time.time() - self.start_time
        if elapsed < 0.1:
            return 0.0

        return (self.bytes_received / 1024) / elapsed

    def print_summary(self):
        """Print statistics summary"""
        elapsed = time.time() - self.start_time
        print("\n" + "="*70)
        print("SESSION SUMMARY")
        print("="*70)
        print(f"Runtime: {elapsed:.1f} seconds")
        print(f"Messages received: {self.messages_received}")
        print(f"Data received: {self.bytes_received:,} bytes ({self.bytes_received/1024:.2f} KB)")

        if elapsed > 0:
            print(f"Average message rate: {self.messages_received/elapsed:.2f} msg/s")
            print(f"Average data rate: {(self.bytes_received/1024)/elapsed:.2f} KB/s")

        if self.rtcm_types_seen:
            print("\nRTCM3 Message Types Received:")
            for msg_type, count in sorted(self.rtcm_types_seen.items()):
                print(f"  {msg_type}: {count} messages")

        print("="*70)

    async def run_async(self):
        """Async run method"""
        print(f"Connecting to Ably...")

        # Initialize Ably client
        self.client = AblyRealtime(self.api_key)

        # Wait for connection
        await self.client.connection.once_async('connected')
        print(f"✓ Connected to Ably")
        print(f"Channel: {self.channel_name}")
        print(f"Listening for RTK correction messages...")
        print(f"Press Ctrl+C to stop\n")

        # Get channel
        self.channel = self.client.channels.get(self.channel_name)

        # Subscribe to all messages on the channel
        await self.channel.subscribe(self.on_message)

        # Keep running
        try:
            while self.running:
                await asyncio.sleep(1)
        except asyncio.CancelledError:
            pass

    def run(self):
        """Start monitoring"""
        try:
            asyncio.run(self.run_async())
        except KeyboardInterrupt:
            print("\n\nStopping monitor...")
            self.running = False
        finally:
            self.print_summary()
            print("Disconnected.")


def parse_credentials_file():
    """Parse API key from ably_credentials.h file"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    creds_file = os.path.join(project_root, "main", "ably_credentials.h")

    if not os.path.exists(creds_file):
        return None, None

    api_key = None
    channel_name = None

    with open(creds_file, 'r') as f:
        for line in f:
            line = line.strip()

            # Look for ABLY_API_KEY_SUBSCRIBER
            if line.startswith('#define ABLY_API_KEY_SUBSCRIBER'):
                parts = line.split('"')
                if len(parts) >= 2:
                    api_key = parts[1]

            # Look for ABLY_CHANNEL_NAME
            elif line.startswith('#define ABLY_CHANNEL_NAME'):
                parts = line.split('"')
                if len(parts) >= 2:
                    channel_name = parts[1]

    return api_key, channel_name


def main():
    print("="*70)
    print("ABLY RTK CHANNEL MONITOR")
    print("="*70)

    # Get API key (environment variable or credentials file)
    api_key = os.environ.get('ABLY_API_KEY')
    channel_name = os.environ.get('ABLY_CHANNEL')

    if not api_key:
        print("Reading credentials from main/ably_credentials.h...")
        api_key, file_channel = parse_credentials_file()

        # Use channel from file if found and not overridden
        if file_channel and not channel_name:
            channel_name = file_channel

        if not api_key:
            print("\nERROR: Could not find ABLY_API_KEY_SUBSCRIBER")
            print("\nOptions:")
            print("  1. Set environment variable: export ABLY_API_KEY='your_key_here'")
            print("  2. Ensure main/ably_credentials.h exists with valid key")
            sys.exit(1)

    if not channel_name:
        channel_name = "rtk:corrections:base1"  # Default

    # Mask API key for security
    if ':' in api_key:
        key_parts = api_key.split(':')
        masked_key = f"{key_parts[0][:8]}...:{key_parts[1][:4]}..."
    else:
        masked_key = f"{api_key[:8]}..."

    print(f"API Key: {masked_key}")
    print(f"Channel: {channel_name}")
    print()

    # Create and run monitor
    try:
        monitor = RTKMonitor(api_key, channel_name)
        monitor.run()
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()
