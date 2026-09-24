#!/usr/bin/env python3
"""Generate synthetic usbmon/pcapng traffic; no device capture is used."""

import pathlib
import struct

HERE = pathlib.Path(__file__).resolve().parent
pack = struct.pack


def block(kind, data):
    padding = b"\0" * (-len(data) % 4)
    size = 12 + len(data) + len(padding)
    return pack("<II", kind, size) + data + padding + pack("<I", size)


packets = []


def exchange(transfer, endpoint, sent, received, setup=None, requested=None):
    sequence = len(packets) // 2 + 1
    for submit, data in ((True, sent), (False, received)):
        length = requested if submit and requested is not None else len(data)
        # Linux usbmon DLT_USB_LINUX_MMAPPED, 64-byte header.
        header = pack("<QBBBBHccqiiII8sIIII",
                      sequence, ord("S" if submit else "C"), transfer, endpoint,
                      1, 1, b"\0" if submit and setup else b"-",
                      b"\0" if data else b"<", 1, 1000 * sequence,
                      -115 if submit else 0, length, len(data),
                      setup if submit and setup else b"\0" * 8, 0, 0, 0, 0)
        packets.append(header + data)


def bulk_out(command):
    exchange(3, 1, command, b"")


def bulk_in(data):
    exchange(3, 0x82, b"", data, requested=2048)


def command(opcode, reply=None, status=b"\0" * 5, param=0, reply_length=None):
    command_bytes = bytearray(12)
    command_bytes[:2] = opcode
    command_bytes[2] = param
    if reply is not None:
        command_bytes[10:12] = pack("<H", reply_length if reply_length is not None else len(reply))
    bulk_out(command_bytes)
    if reply is not None:
        bulk_in(reply)
    if status is not None:
        bulk_in(status)


# Standard USB language and product string requests used by the probe.
exchange(2, 0x80, b"", b"\x04\x03\x09\x04",
         setup=pack("<BBHHH", 0x80, 6, 0x0300, 0, 4), requested=4)
label = "Synthetic Kensington".encode("utf-16le")
string = bytes((len(label) + 2, 3)) + label
exchange(2, 0x80, b"", string, setup=pack("<BBHHH", 0x80, 6, 0x0302, 0x0409, 255), requested=255)
# Device info: host-bound flag set. Select OS; ten storage slots.
exchange(2, 0x80, b"", b"\x01" + b"\0" * 7,
         setup=pack("<BBHHH", 0xc0, 7, 0x000d, 0, 8), requested=8)
bulk_out(b"\x05\x13\x01" + bytes(9))
bulk_in(b"\0" * 5)
command(b"\x45\x0d", b"\0\x0a")

# Listing an empty reader still issues a ten-slot storage read.
command(b"\x45\x0e", bytes(10 * 52))

identifier = b"FP1-123456789012345678901234"  # fictional ID, no biometric data
assert len(identifier) == 28
records = bytearray(10 * 52)
records[0] = 1
records[2] = 0xff
records[3:31] = identifier
records[35] = 0
records[36:52] = bytes(range(1, 17))  # deliberately synthetic UUID

# List once more with one print, then verify it. Capture polls once while
# waiting and once on finger detection; 45/22 has no following status read.
command(b"\x45\x0e", records)
command(b"\x45\x0e", records)
command(b"\x05\x05", param=2)
command(b"\x45\x06", b"\x01" + bytes(4))
command(b"\x45\x06", bytes(5))
command(b"\x45\x08", bytes(9), param=2)
command(b"\x45\x22", b"\x00\xff" + identifier + bytes(12), status=None)

# Reject malformed occupancy and a truncated storage reply, without any
# commands that modify on-device prints.
malformed = bytearray(records)
malformed[0] = 2
command(b"\x45\x0e", malformed)
command(b"\x45\x0e", bytes(10 * 52 - 1), status=None, reply_length=10 * 52)

capture = block(0x0a0d0d0a, pack("<IHHq", 0x1a2b3c4d, 1, 0, -1))
capture += block(1, pack("<HHI", 220, 0, 65535))
for packet in packets:
    capture += block(6, pack("<IIIII", 0, 0, 0, len(packet), len(packet)) + packet)
(HERE / "custom.pcapng").write_bytes(capture)
