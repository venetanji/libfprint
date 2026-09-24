#!/usr/bin/env python3

import gi
gi.require_version('FPrint', '2.0')
from gi.repository import FPrint

import sys
import traceback
sys.excepthook = lambda *args : (traceback.print_exception(*args), sys.exit(1))


c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "syna0082"
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)

d.open_sync()

template = FPrint.Print.new(d)

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

print("enrolling")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
p = d.enroll_sync(template, None, enroll_progress, None)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("enroll done")

print("verifying (same finger, expect match)")
verify_res, verify_print = d.verify_sync(p)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify done")
assert verify_res == True

d.close_sync()

del d
del c
