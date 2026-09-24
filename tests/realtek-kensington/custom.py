#!/usr/bin/env python3
"""Only synthetic, empty storage: never issue a clear or delete command."""

import gi

gi.require_version("FPrint", "2.0")
from gi.repository import FPrint, GLib

context = FPrint.Context()
context.enumerate()
devices = context.get_devices()
assert len(devices) == 1
device = devices[0]
assert device.get_driver() == "realtek"
device.open_sync()
assert len(device.list_prints_sync()) == 0
prints = device.list_prints_sync()
assert len(prints) == 1
matched, scan = device.verify_sync(prints[0])
assert matched and scan.equal(prints[0])
for _ in range(2):
    try:
        device.list_prints_sync()
    except GLib.Error:
        pass
    else:
        raise AssertionError("Malformed storage response was accepted")
device.close_sync()
