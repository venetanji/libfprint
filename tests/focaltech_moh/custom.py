#!/usr/bin/python3

import traceback
import sys

import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint

# Exit with error on any exception, included those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "focaltech_moh"
assert d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert not d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)

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
print("verify done")
assert verify_res == True

d.close_sync()

del d
del c
