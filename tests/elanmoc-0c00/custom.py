#!/usr/bin/python3

import sys
import traceback

import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, included those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "elanmoc2"
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)

d.open_sync()

# The test aims to stress the "get enrolled count" command. Some devices occasionally respond with
# an empty payload to this command and the driver should handle this gracefully by retrying the command.

print("clearing device storage")
d.clear_storage_sync()

print("ensuring device storage is empty")
stored = d.list_prints_sync()
assert len(stored) == 0

d.close_sync()
del d
del c
del ctx
