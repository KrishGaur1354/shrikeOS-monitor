#!/usr/bin/env python3
"""
ShrikeOS Monitor — Serial-to-WebSocket Bridge

Bridges the Shrike-lite USB serial port to a WebSocket server
so any browser (including Chromium) can communicate with the board.

Usage:
    python3 bridge.py [--port /dev/ttyACM0] [--ws-port 8765]

Then open the dashboard — it will auto-connect to ws://localhost:8765
"""

import asyncio
import argparse
import json
import signal
import sys

import serial
import websockets

# Global state
serial_port = None
ws_clients = set()


def open_serial(port_name, baud=115200):
    """Open the serial port to the Shrike-lite board."""
    try:
        ser = serial.Serial(port_name, baud, timeout=0.1)
        print(f"[BRIDGE] Serial opened: {port_name} @ {baud} baud")
        return ser
    except serial.SerialException as e:
        print(f"[BRIDGE] ERROR: Cannot open {port_name}: {e}")
        print(f"[BRIDGE] Try: sudo chmod 666 {port_name}")
        sys.exit(1)


async def serial_reader(ser):
    """Read lines from serial and broadcast to all WebSocket clients."""
    loop = asyncio.get_event_loop()
    buf = b""

    while True:
        try:
            data = await loop.run_in_executor(None, ser.read, 256)
            if data:
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line_str = line.decode("utf-8", errors="replace").strip()
                    if line_str:
                        # Broadcast to all connected WebSocket clients
                        if ws_clients:
                            await asyncio.gather(
                                *[client.send(line_str) for client in ws_clients],
                                return_exceptions=True,
                            )
            else:
                await asyncio.sleep(0.05)
        except Exception as e:
            print(f"[BRIDGE] Serial read error: {e}")
            await asyncio.sleep(1)


async def ws_handler(websocket):
    """Handle a single WebSocket client connection."""
    ws_clients.add(websocket)
    remote = websocket.remote_address
    print(f"[BRIDGE] Dashboard connected from {remote}")

    try:
        async for message in websocket:
            # Forward commands from browser to serial
            msg = message.strip()
            if msg and serial_port and serial_port.is_open:
                serial_port.write((msg + "\n").encode("utf-8"))
                print(f"[BRIDGE] TX → Board: {msg}")
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        ws_clients.discard(websocket)
        print(f"[BRIDGE] Dashboard disconnected from {remote}")


async def main(serial_dev, ws_port):
    global serial_port

    serial_port = open_serial(serial_dev)

    # Start WebSocket server
    print(f"[BRIDGE] WebSocket server on ws://localhost:{ws_port}")
    print(f"[BRIDGE] Open the dashboard and it will auto-connect!")
    print(f"[BRIDGE] Press Ctrl+C to stop\n")

    async with websockets.serve(ws_handler, "localhost", ws_port):
        await serial_reader(serial_port)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ShrikeOS Serial-WebSocket Bridge")
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("--ws-port", type=int, default=8765, help="WebSocket port")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.port, args.ws_port))
    except KeyboardInterrupt:
        print("\n[BRIDGE] Stopped.")
        if serial_port:
            serial_port.close()
