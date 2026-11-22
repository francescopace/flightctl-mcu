#!/usr/bin/env python3
"""Connect one USB serial port to a private gvproxy user-mode network."""
import argparse
import os
from pathlib import Path
import secrets
import selectors
import socket
import struct
import subprocess
import tempfile
import time

import serial
from serial.tools.list_ports import comports


# Default gateway and reserved guest addresses in gvproxy v0.9.0.
GATEWAY_MAC = bytes.fromhex('5a94efe40cdd')
GUEST_MAC = bytes.fromhex('5a94efe40cee')
GUEST_IP = socket.inet_aton('192.168.127.2')
IPV4 = b'\x08\x00'
ARP = b'\x08\x06'


def port_for(serial_number, device=None):
    matches = [p.device for p in comports()
               if p.serial_number == serial_number
               and (p.vid, p.pid) in ((0x0483, 0x374b), (0x1366, 0x1051), (0x1366, 0x1061))
               and (device is None or device == p.device)]
    if len(matches) != 1:
        raise SystemExit(f"Expected one probe port for USB serial {serial_number}. "
                         "Use --port to select the application UART.")
    return matches[0]


def send_frame(net, frame):
    net.sendall(struct.pack('!I', len(frame)) + frame)


def ipv4_packet(data):
    if len(data) < 20 or data[0] >> 4 != 4:
        return b''
    size = int.from_bytes(data[2:4], 'big')
    header = (data[0] & 15) * 4
    return data[:size] if 20 <= header <= size <= min(1500, len(data)) else b''


def from_gateway(frame, net):
    if len(frame) >= 14 and frame[12:14] == IPV4 and frame[:6] == GUEST_MAC:
        return ipv4_packet(frame[14:])
    # SLIP carries IP packets. Answer the private gateway's Ethernet ARP
    # request on behalf of its one statically addressed serial peer.
    if (len(frame) >= 42 and frame[12:14] == ARP and
            frame[14:22] == bytes.fromhex('0001080006040001') and frame[38:42] == GUEST_IP):
        sender = frame[22:28]
        send_frame(net, sender + GUEST_MAC + ARP + frame[14:20] + b'\x00\x02' +
                   GUEST_MAC + GUEST_IP + sender + frame[28:32])
    return b''


def forward(uart, net, ipv4=False, pace_uart=False):
    serial_rx = bytearray()
    net_rx = bytearray()
    with selectors.DefaultSelector() as poll:
        poll.register(uart, selectors.EVENT_READ)
        poll.register(net, selectors.EVENT_READ)
        while True:
            for key, _ in poll.select():
                if key.fileobj is uart:
                    serial_rx.extend(uart.read(max(1, uart.in_waiting)))
                    while b'\xc0' in serial_rx:
                        boundary = serial_rx.index(0xc0)
                        frame = bytes(serial_rx[:boundary])
                        del serial_rx[:boundary + 1]
                        frame = frame.replace(b'\xdb\xdc', b'\xc0').replace(b'\xdb\xdd', b'\xdb')
                        if ipv4:
                            packet = ipv4_packet(frame)
                            if packet and len(packet) == len(frame) and packet[12:16] == GUEST_IP:
                                send_frame(net, GATEWAY_MAC + GUEST_MAC + IPV4 + packet)
                        elif 14 <= len(frame) <= 1518 and frame[12:14] in (b'\x08\x00', b'\x08\x06'):
                            send_frame(net, frame)
                    if len(serial_rx) > 4096:
                        serial_rx.clear()
                else:
                    chunk = net.recv(65536)
                    if not chunk:
                        raise ConnectionError("gvproxy disconnected")
                    net_rx.extend(chunk)
                    while len(net_rx) >= 4:
                        size, = struct.unpack_from('!I', net_rx)
                        if size > 65535:
                            raise ValueError("invalid Ethernet frame size from gvproxy")
                        if len(net_rx) < 4 + size:
                            break
                        frame = bytes(net_rx[4:4 + size])
                        del net_rx[:4 + size]
                        if ipv4:
                            frame = from_gateway(frame, net)
                            if not frame:
                                continue
                        wire = b'\xc0' + frame.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc') + b'\xc0'
                        if pace_uart:
                            # Allow the Nordic UART to receive while the IP stack handles the
                            # preceding burst. Certificate records regularly fill the MTU.
                            for start in range(0, len(wire), 32):
                                uart.write(wire[start:start + 32])
                                time.sleep(0.002)
                        else:
                            uart.write(wire)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True, help='USB serial number of the debug probe')
    ap.add_argument('--port', help='virtual COM port when the probe exposes multiple UARTs')
    ap.add_argument('--link', choices=('ethernet', 'ipv4'), default='ethernet',
                    help='Zephyr SLIP TAP uses ethernet, NuttX SLIP uses ipv4')
    ap.add_argument('--gvproxy', default='gvproxy')
    ap.add_argument('--pace-uart', action='store_true',
                    help='pace writes to the Nordic J-Link virtual serial port')
    args = ap.parse_args()
    device = port_for(args.serial, args.port)
    with tempfile.TemporaryDirectory(prefix='fc-st-', dir='/tmp') as tmp:
        token = Path(tmp, 'token')
        with os.fdopen(os.open(token, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as f:
            f.write(secrets.token_hex(32))
        path = str(Path(tmp, 'net'))
        gateway = subprocess.Popen([args.gvproxy, '-listen', 'unix://' + tmp + '/api',
                                    '-listen-qemu', 'unix://' + path, '-ssh-port', '-1',
                                    '-api-token-file', str(token)])
        try:
            with socket.socket(socket.AF_UNIX) as net:
                for attempt in range(100):
                    try:
                        net.connect(path)
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if gateway.poll() is not None or attempt == 99:
                            raise
                        time.sleep(0.05)
                with serial.Serial(device, 460800, timeout=0, exclusive=True) as uart:
                    print(f"USB network ready on {device} ({args.serial})", flush=True)
                    forward(uart, net, args.link == 'ipv4', args.pace_uart)
        finally:
            gateway.terminate()
            gateway.wait()


if __name__ == '__main__':
    main()
