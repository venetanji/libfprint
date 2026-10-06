#!/usr/bin/env python3

import sys
import traceback

import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, including those in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == 'cv2'
d.open_sync()

retries = 0


def enroll_progress(dev, stage, pnt, data, error):
    global retries
    if error:
        assert error.matches(FPrint.DeviceRetry.quark(), FPrint.DeviceRetry.GENERAL), error
        retries += 1
    print('enroll progress: stage %d, error %s' % (stage, error))


def enroll(finger):
    template = FPrint.Print.new(d)
    template.set_finger(finger)
    p = d.enroll_sync(template, None, enroll_progress, None)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    return p


print('>>> Enroll RIGHT INDEX: first just TAP the reader very briefly, then press normally')
p_right = enroll(FPrint.Finger.RIGHT_INDEX)
assert retries >= 1, 'record again: the first tap was not short enough'

print('>>> Verify RIGHT INDEX: just TAP the reader very briefly')
try:
    d.verify_sync(p_right)
    raise AssertionError('record again: the tap was not short enough')
except GLib.Error as e:
    assert e.matches(FPrint.DeviceRetry.quark(), FPrint.DeviceRetry.GENERAL), e
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE

print('>>> Enroll LEFT INDEX')
p_left = enroll(FPrint.Finger.LEFT_INDEX)

print('deleting the right print on the reader only')
d.delete_print_sync(p_right)

# The missing print comes first, so the one-at-a-time fallback has to skip it
# before it finds the left print.
gallery = [FPrint.Print.deserialize(p.serialize()) for p in (p_right, p_left)]

print('>>> Identify with the missing print first: press LEFT INDEX')
match, _ = d.identify_sync(gallery)
assert match is not None and match.equal(p_left)

print('deleting the left print on the reader too')
d.delete_print_sync(p_left)

print('>>> Identify with every print missing: press LEFT INDEX')
try:
    d.identify_sync(gallery)
    raise AssertionError('identify with every print missing succeeded')
except GLib.Error as e:
    assert e.matches(FPrint.DeviceError.quark(), FPrint.DeviceError.DATA_NOT_FOUND), e

d.close_sync()

del d
del c
