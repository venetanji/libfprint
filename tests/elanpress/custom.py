#!/usr/bin/env python3
# Starting point for tests/elanpress/custom.py in libfprint, to run against
# the umockdev recording produced by tests/create-driver-test.py elanpress.

import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()
d = devices[0]
del devices

assert d.get_driver() == "elanpress"
assert d.get_scan_type() == FPrint.ScanType.PRESS
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)

d.open_sync()
assert d.get_nr_enroll_stages() == 20

template = FPrint.Print.new(d)

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

print("enrolling")
p = d.enroll_sync(template, None, enroll_progress, None)
print("enroll done")

print("verifying")
verify_res, verify_print = d.verify_sync(p)
assert verify_res == True
print("verify done")

print("identifying")
match, print_ = d.identify_sync([FPrint.Print.deserialize(p.serialize())])
assert match is not None
print("identify done")

d.close_sync()
del d
del c
