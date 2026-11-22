"""NuttX uses IP over SLIP on the same virtual Ethernet network as Zephyr."""
import importlib.util
from pathlib import Path
import socket
import struct
import unittest
from unittest.mock import patch
from types import SimpleNamespace

path = Path(__file__).resolve().parents[2] / "port/zephyr/stlink-network.py"
spec = importlib.util.spec_from_file_location("serial_network", path)
bridge = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(bridge)
except ModuleNotFoundError as error:
    if error.name != "serial":
        raise
    raise unittest.SkipTest("USB bridge tests require pySerial") from error


class SerialNetworkTest(unittest.TestCase):
    def test_arp_for_serial_peer(self):
        gw = bridge.GATEWAY_MAC
        ip = socket.inet_aton("192.168.127.1")
        header = bytes.fromhex("000108000604")
        payload = header + b"\x00\x01" + gw + ip + bytes(6) + bridge.GUEST_IP
        request = b"\xff" * 6 + gw + bridge.ARP + payload
        outgoing, incoming = socket.socketpair()
        with outgoing, incoming:
            self.assertEqual(bridge.from_gateway(request, outgoing), b"")
            expected = (gw + bridge.GUEST_MAC + bridge.ARP + header + b"\x00\x02" +
                        bridge.GUEST_MAC + bridge.GUEST_IP + gw + ip)
            self.assertEqual(incoming.recv(128), struct.pack("!I", len(expected)) + expected)

    def test_ipv4_length_excludes_ethernet_padding(self):
        packet = bytes.fromhex("45000014") + bytes(16)
        frame = bridge.GUEST_MAC + bridge.GATEWAY_MAC + bridge.IPV4 + packet + bytes(26)
        self.assertEqual(bridge.from_gateway(frame, None), packet)
        self.assertEqual(bridge.ipv4_packet(packet[:-1]), b"")
        self.assertEqual(bridge.ipv4_packet(b"\x4f" + packet[1:]), b"")

    def test_probe_serial_and_port_must_both_match(self):
        ports = [SimpleNamespace(device=name, serial_number="123", vid=0x1366, pid=0x1061)
                 for name in ("/dev/a", "/dev/b")]
        with patch.object(bridge, "comports", return_value=ports):
            self.assertEqual(bridge.port_for("123", "/dev/b"), "/dev/b")
            with self.assertRaises(SystemExit):
                bridge.port_for("123")
            with self.assertRaises(SystemExit):
                bridge.port_for("456", "/dev/b")
