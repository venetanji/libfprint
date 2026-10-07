#!/usr/bin/env python3

import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, included those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "goodix5125"
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)

d.open_sync()
assert d.get_nr_enroll_stages() == 12

# An empty gallery (fprintd's duplicate check before the first enrolment)
# is no match, without waiting for a finger
print("identifying against an empty gallery")
match, _ = d.identify_sync([])
assert match is None

progress = []

def enroll_progress(device, completed_stages, print_, *args):
    print('enroll progress: ' + str((completed_stages,) + args))
    progress.append(completed_stages)

print("enrolling, touch the sensor 12 times")
template = FPrint.Print.new(d)
template.set_finger(FPrint.Finger.LEFT_LITTLE)
p = d.enroll_sync(template, None, enroll_progress, None)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
assert p.get_finger() == FPrint.Finger.LEFT_LITTLE
assert max(progress) == 12
print("enroll done")
del template

# Verify with a stored copy, like fprintd does
stored = FPrint.Print.deserialize(p.serialize())
del p

# A single touch may land on a part of the finger that was not enrolled;
# the recording keeps every attempt, so the replay takes the same path
for attempt in range(5):
    print("verifying, touch the sensor with the enrolled finger")
    verify_res, verify_print = d.verify_sync(stored)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    print("verify done: " + ("match" if verify_res else "no match"))
    if verify_res:
        break
assert verify_res == True

d.close_sync()

del d
del c
