#!/usr/bin/env python3

import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, included those happening in async callbacks
# except we handle it manually in verify
# sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "canvasbio"
assert d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)

d.open_sync()

template = FPrint.Print.new(d)

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

# Enroll
print("enrolling")
p = d.enroll_sync(template, None, enroll_progress, None)
print("enroll done")

# Verify
print("verifying")
verify_res, verify_print = d.verify_sync(p)
if verify_res:
    print("verify done (MATCH)")
else:
    print("verify done (NO MATCH)")

assert verify_res == True

d.close_sync()

del d
del c
