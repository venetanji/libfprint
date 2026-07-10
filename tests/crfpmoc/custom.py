#!/usr/bin/python3

import traceback
import sys
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

assert d.get_driver() == "crfpmoc"
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)

d.open_sync()


def enroll_progress(*args):
    print("finger status: ", d.get_finger_status())
    print('enroll progress: ' + str(args))


def enroll_finger(finger):
    template = FPrint.Print.new(d)
    template.set_finger(finger)
    print("enrolling", finger)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    p = d.enroll_sync(template, None, enroll_progress, None)
    assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
    print("enroll done:", p)
    return p

def identify_done(dev, res):
    global identified
    identified = True
    identify_match, identify_print = dev.identify_finish(res)
    print('identification_done: ', identify_match, identify_print)
    assert identify_match is not None
    assert identify_print is not None
    assert identify_match != identify_print
    assert identify_match == deserialized_prints[-1]
    assert identify_match.equal(identify_print)


# clear, identify (nothing enrolled), enroll, verify, identify (bad gallery),
# enroll another finger, verify the first, clear

print("clear device storage")
d.clear_storage_sync()
print("clear done")

# Enroll a first finger up front so we have valid, uploadable prints to build
# galleries from. (A print with no template data cannot be uploaded to the
# sensor, so identify galleries must carry real template data.)
p1 = enroll_finger(FPrint.Finger.LEFT_INDEX)

gallery_a = [FPrint.Print.deserialize(p1.serialize())]

# Identify while the device store is empty: clear the device and identify a
# gallery of valid prints. The device reports no enrolled templates, so no
# match is reported (and no error is raised).
print("clear device storage")
d.clear_storage_sync()
print("clear done")

print('identifying with nothing enrolled')
identify_match, identify_print = d.identify_sync(gallery_a)
print('identification (empty store) done:', identify_match, identify_print)
assert identify_match is None
del gallery_a

# Re-enroll the first finger so the device has it stored again, then verify.
p1 = enroll_finger(FPrint.Finger.LEFT_INDEX)

print("verifying first finger")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
verify_res, verify_print = d.verify_sync(p1)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify done:", verify_res, verify_print)
assert verify_res == True
assert verify_print is not None
assert p1 != verify_print
assert verify_print.equal(p1)

# Identify (async) finger A against a gallery that contains it: a match must
# be reported and the scanned print must equal the matched gallery entry.
identified = False
deserialized_prints = [FPrint.Print.deserialize(p1.serialize())]
assert deserialized_prints[-1].equal(p1)

print('async identifying')
d.identify(deserialized_prints, callback=identify_done)
while not identified:
    ctx.iteration(True)
del deserialized_prints

# Identify against a gallery of prints whose finger the sensor does not
# match: no gallery entry corresponds to the scanned finger, so no match is
# reported. The gallery entries carry valid (but different) template data so
# they can still be uploaded to the sensor.
identified = False
identify_result = None


def identify_no_match_done(dev, res):
    global identified, identify_result
    identified = True
    identify_result = dev.identify_finish(res)


p2 = enroll_finger(FPrint.Finger.RIGHT_INDEX)
assert not p1.equal(p2)
not_enrolled_gallery = [FPrint.Print.deserialize(p2.serialize()) for _ in range(3)]

print('identifying with a non-matching gallery')
d.identify(not_enrolled_gallery, callback=identify_no_match_done)
while not identified:
    ctx.iteration(True)
identify_match, identify_print = identify_result
print('identification (non-matching gallery) done:', identify_match, identify_print)
assert identify_match is None
del not_enrolled_gallery

print("verifying first finger again")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
verify_res, verify_print = d.verify_sync(p1)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify (after second enroll) done:", verify_res, verify_print)
assert verify_res == True
assert verify_print is not None
assert verify_print.equal(p1)

del p1
del p2

print("clear device storage")
d.clear_storage_sync()
print("clear done")

d.close_sync()

del d
del c
