#!/usr/bin/env python3
"""BLE field provisioning against an emulated device, as an installer's phone
would do it: Google Bumble as the BLE controller for the emulator (esp-emu
--ble-hci tcp:HOST:9544) and as the phone, ESP-IDF's esp_prov for the
session (Security 1, proof of possession) and the WiFi messages.

    python3 tests/e2e/ble-prov.py --pop SECRET --ssid NAME --passphrase PASS [--wrong-pop OTHER]

Start it before the emulator: it listens on port 9544. It scans for a device
advertising FC-..., connects, and maps the provisioning endpoints from the
characteristics' user descriptions, as esp_prov does. With --wrong-pop it
first checks that a session with that proof is refused. Once the network is
given it stays, as the controller a device has would: until the device has
closed the connection and shut its BLE down, or --stay seconds. Needs bumble,
protobuf and cryptography, $ESP_PROV_DIR (esp_prov) and $IDF_PATH (protocomm); tests/e2e/ble-prov.sh
runs it in the espressif/idf image.
"""
import argparse
import asyncio
import os
import sys

sys.path.insert(0, os.environ["ESP_PROV_DIR"])
import esp_prov  # noqa: E402
import security  # noqa: E402

from bumble import att, hci  # noqa: E402
from bumble.core import UUID  # noqa: E402
from bumble.controller import Controller  # noqa: E402
from bumble.device import Device, Peer  # noqa: E402
from bumble.host import Host  # noqa: E402
from bumble.link import LocalLink  # noqa: E402
from bumble.transport import open_transport  # noqa: E402


def ack_privacy_commands():
    """NimBLE sends LL privacy housekeeping that Bumble's controller does not
    answer; ack it, as the emulator's tools/bumble_test.py does."""
    def ack_ok(self, command):
        params = hci.HCI_GenericReturnParameters(data=bytes([0]))
        if isinstance(command, hci.HCI_SyncCommand):
            return params
        self.send_hci_packet(hci.HCI_Command_Complete_Event(
            num_hci_command_packets=1, command_opcode=command.op_code, return_parameters=params))
        return None

    for name in ("on_hci_le_set_privacy_mode_command", "on_hci_le_clear_resolving_list_command",
                 "on_hci_le_add_device_to_resolving_list_command",
                 "on_hci_le_remove_device_from_resolving_list_command"):
        if not hasattr(Controller, name):
            setattr(Controller, name, ack_ok)


PROV_SERVICE = "1775244d-6b43-439b-877c-060f2d9bed07"
# A device that has nothing more to ask its controller for this long has shut
# its BLE down (seconds here; the emulator runs slower than that).
QUIET_S = 15


async def stay(closed, controller, limit):
    """After provisioning the device ends the connection and shuts its BLE
    down, which it asks its controller for. This one used to leave as soon as
    the network was given, closing the emulator's HCI link under a host that
    was still using it: the emulated device then stopped for minutes, at
    times. It answers until the device is done."""
    loop = asyncio.get_running_loop()
    last = loop.time()
    on_packet = controller.on_packet

    def seen(packet):
        nonlocal last
        last = loop.time()
        on_packet(packet)

    controller.on_packet = seen
    start = loop.time()
    while loop.time() - start < limit:
        if closed.is_set() and loop.time() - last >= QUIET_S:
            print(f"the device shut its BLE down ({loop.time() - start:.0f} s after the network was given)", flush=True)
            return
        await asyncio.sleep(1)
    print(f"the device still used BLE after {limit} s: leaving it", flush=True)


class Transport:
    """esp_prov's transport interface over a Bumble GATT connection."""

    def __init__(self, chars):
        self.chars = chars  # endpoint name -> characteristic

    async def send_data(self, ep_name, data):
        c = self.chars[ep_name]
        await c.write_value(bytes(data.encode("latin-1")), with_response=True)
        return "".join(chr(b) for b in await c.read_value())


async def connect(phone, prefix, timeout):
    found = asyncio.get_running_loop().create_future()

    seen = set()

    def on_adv(adv):
        if str(adv.address) not in seen:
            seen.add(str(adv.address))
            print(f"advertisement from {adv.address}", flush=True)
        name = adv.data.get(0x09) or adv.data.get(0x08) or b""
        name = name.decode(errors="replace") if isinstance(name, bytes) else str(name)
        # The name is in the scan response, which Bumble's virtual controller
        # does not pass on: the device is known by the provisioning service
        # it announces (ESP-IDF's default UUID).
        uuids = [str(u).lower() for u in (adv.data.get(0x07) or [])]
        if (name.startswith(prefix) or PROV_SERVICE in uuids) and not found.done():
            found.set_result((adv.address, name))

    phone.on("advertisement", on_adv)
    await phone.start_scanning()
    address, name = await asyncio.wait_for(found, timeout)
    await phone.stop_scanning()
    print(f"found {name} at {address}", flush=True)
    connection = await phone.connect(address, timeout=15)
    peer = Peer(connection)
    # As phones do: messages fit in one write (NimBLE refuses long writes).
    print("MTU", await peer.request_mtu(512), flush=True)
    await peer.discover_services()
    print(f"connected to {address}", flush=True)
    chars = {}
    for service in peer.services:
        if str(service.uuid).lower() != PROV_SERVICE:
            continue
        await service.discover_characteristics()
        for c in service.characteristics:
            await c.discover_descriptors()
            for d in c.descriptors:
                if d.type == UUID.from_16_bits(0x2901):
                    chars[bytes(await d.read_value()).decode().lower()] = c
    print("endpoints:", ", ".join(sorted(chars)), flush=True)
    return connection, Transport(chars)


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("--pop", required=True)
    p.add_argument("--wrong-pop")
    p.add_argument("--ssid", required=True)
    p.add_argument("--passphrase", default="")
    p.add_argument("--port", default="9544")
    p.add_argument("--timeout", type=int, default=180)
    p.add_argument("--stay", type=int, default=240,
                   help="seconds to go on as the device's controller, at most, once the network is given")
    a = p.parse_args()

    ack_privacy_commands()
    link = LocalLink()
    emu = await open_transport(f"tcp-server:_:{a.port}")
    emu_ctrl = Controller("emu", host_source=emu.source, host_sink=emu.sink, link=link,
                          public_address="00:11:22:33:44:55")
    emu_ctrl.random_address = hci.Address("00:11:22:33:44:55", hci.Address.PUBLIC_DEVICE_ADDRESS)
    phone_ctrl = Controller("phone", link=link, public_address="AA:BB:CC:DD:EE:FF")
    host = Host()
    host.controller, phone_ctrl.host = phone_ctrl, host
    phone = Device(name="installer", host=host)
    await phone.power_on()
    print(f"BLE controller for the emulator on port {a.port}", flush=True)

    connection, tp = await connect(phone, "FC-", a.timeout)
    print("protocol:", await esp_prov.get_version(tp), flush=True)
    if a.wrong_pop:
        try:
            ok = await esp_prov.establish_session(tp, security.Security1(a.wrong_pop, False))
        except att.ATT_Error as e:  # the device ends the session on a wrong proof
            print("session refused:", e.error_code, flush=True)
            ok = False
        if ok:
            sys.exit("session established with a wrong proof of possession")
        print("wrong proof of possession refused", flush=True)
        await connection.disconnect()
        connection, tp = await connect(phone, "FC-", a.timeout)
    closed = asyncio.Event()
    connection.on("disconnection", lambda *_: closed.set())
    sec = security.Security1(a.pop, False)
    if not await esp_prov.establish_session(tp, sec):
        sys.exit("no session with the right proof of possession")
    if not await esp_prov.send_wifi_config(tp, sec, a.ssid, a.passphrase):
        sys.exit("WiFi network not accepted")
    if not await esp_prov.apply_wifi_config(tp, sec):
        sys.exit("WiFi network not applied")
    for _ in range(30):
        state = await esp_prov.get_wifi_config(tp, sec)
        print("WiFi state:", state, flush=True)
        if state == "connected":
            print("Provisioning was successful", flush=True)
            await stay(closed, emu_ctrl, a.stay)
            return
        if state == "failed":
            sys.exit("device could not join the network")
        await asyncio.sleep(2)
    sys.exit("device did not connect")


if __name__ == "__main__":
    asyncio.run(main())
