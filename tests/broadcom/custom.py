#!/usr/bin/python3

import traceback
import sys
import os
import shutil
import tempfile
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, including those in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

session_dir_base = os.environ.get('G_TEST_BUILDDIR', tempfile.gettempdir())
session_dir = os.path.join(session_dir_base, 'broadcom-session-cache')
shutil.rmtree(session_dir, ignore_errors=True)
os.environ['FP_BROADCOM_SESSION_DIR'] = session_dir

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "broadcom"
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)

d.open_sync()

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

def enroll_finger(finger):
    template = FPrint.Print.new(d)
    template.set_finger(finger)
    print("enrolling", finger)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    enrolled = d.enroll_sync(template, None, enroll_progress, None)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    print("enroll done:", enrolled)
    return enrolled

def identify_done(dev, res):
    global identified, identify_match

    identified = True
    identify_match, identify_print = dev.identify_finish(res)
    print('identification_done. match:', identify_match,
          'print:', identify_print)
    assert identify_match is not None
    assert identify_print is not None
    assert identify_match.equal(identify_print)

# Enroll, verify, identify, delete. Broadcom cannot list or clear storage; the
# serialized print carries a device-side template ID, not uploadable template data.
enrolled = enroll_finger(FPrint.Finger.LEFT_INDEX)

print("verifying enrolled finger")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
verify_res, verify_print = d.verify_sync(enrolled)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify done:", verify_res, verify_print)
assert verify_res == True
assert verify_print is not None
assert verify_print.equal(enrolled)

identified = False
identify_match = None
deserialized_prints = [FPrint.Print.deserialize(enrolled.serialize())]
assert deserialized_prints[-1].equal(enrolled)

print('async identifying')
d.identify(deserialized_prints, callback=identify_done)
while not identified:
    ctx.iteration(True)
del deserialized_prints

assert identify_match.equal(enrolled)

print("deleting")
d.delete_print_sync(enrolled)
print("delete done")

del enrolled

d.close_sync()

del d
del c
