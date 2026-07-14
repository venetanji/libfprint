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
assert d.has_feature(FPrint.DeviceFeature.STORAGE)           # list stub + delete registered
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)    # sends DELETE_OBJECT to device

d.open_sync()

template = FPrint.Print.new(d)

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

def identify_done(dev, res):
    global identified
    identified = True
    identify_match, identify_print = dev.identify_finish(res)
    print('identification_done: ', identify_match, identify_print)
    assert identify_match is not None
    assert identify_match.equal(identify_print)

# Enroll
print("enrolling")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
p = d.enroll_sync(template, None, enroll_progress, None)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("enroll done")

# Verify
print("verifying")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
verify_res, verify_print = d.verify_sync(p)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify done")
assert verify_res == True

# Identify (async, like goodixmoc test)
identified = False
deserialized_prints = [FPrint.Print.deserialize(p.serialize())]
assert deserialized_prints[0].equal(p)

print('async identifying')
d.identify(deserialized_prints, callback=identify_done)
del deserialized_prints

while not identified:
    ctx.iteration(True)

print("deleting")
d.delete_print_sync(p)
print("delete done")

d.close_sync()

del d
del c
