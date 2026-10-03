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
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)

d.open_sync()

retries = 0


def enroll_progress(dev, stage, pnt, data, error):
    global retries
    if error:
        retries += 1
    print('enroll progress: stage %d, error %s' % (stage, error))


def enroll(finger, prompt):
    global retries
    retries = 0
    print(prompt)
    template = FPrint.Print.new(d)
    template.set_finger(finger)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    p = d.enroll_sync(template, None, enroll_progress, None)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    return p, retries


print('a print without template data is rejected without using the reader')
try:
    d.verify_sync(FPrint.Print.new(d))
    raise AssertionError('verify with an empty print succeeded')
except GLib.Error as e:
    assert e.matches(FPrint.DeviceError.quark(), FPrint.DeviceError.DATA_INVALID), e

p_right, r = enroll(FPrint.Finger.RIGHT_INDEX,
                    '>>> Enroll RIGHT INDEX')
print('rejected samples during enrollment: %d' % r)

print('>>> Verify: press RIGHT INDEX')
ok, _ = d.verify_sync(p_right)
assert ok

print('>>> Verify: press RIGHT THUMB (not enrolled)')
ok, _ = d.verify_sync(p_right)
assert not ok
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE

p_left, _ = enroll(FPrint.Finger.LEFT_INDEX, '>>> Enroll LEFT INDEX')

gallery = [FPrint.Print.deserialize(p.serialize()) for p in (p_right, p_left)]

print('>>> Identify: press LEFT INDEX')
match, _ = d.identify_sync(gallery)
assert match is not None and match.equal(p_left)

print('>>> Identify: press RIGHT THUMB (not enrolled)')
match, _ = d.identify_sync(gallery)
assert match is None

print('deleting the left print on the reader only')
d.delete_print_sync(p_left)

# fprintd still has the left print. The reader fails a match whose list
# holds a missing template; the driver must still find the right print.
print('>>> Identify with a stale print in the gallery: press RIGHT INDEX')
match, _ = d.identify_sync(gallery)
assert match is not None and match.equal(p_right)

print('a stale print alone is reported as not found')
print('>>> Verify against the deleted print: press RIGHT INDEX')
try:
    d.verify_sync(p_left)
    raise AssertionError('verify against a deleted print succeeded')
except GLib.Error as e:
    assert e.matches(FPrint.DeviceError.quark(), FPrint.DeviceError.DATA_NOT_FOUND), e

print('deleting the right print')
d.delete_print_sync(p_right)

d.close_sync()

del d
del c
