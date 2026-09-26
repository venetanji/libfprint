#!/usr/bin/python3

import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, including those happening in callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

assert len(devices) == 1, f"Expected 1 device, got {len(devices)}"

d = devices[0]
del devices

# Verify driver name
assert d.get_driver() == "validity", f"Expected 'validity', got '{d.get_driver()}'"

# Verify features detected by auto_initialize_features
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)
assert d.has_feature(FPrint.DeviceFeature.ALWAYS_ON)

d.open_sync()

# 1. Clear storage — ensure the sensor is in a clean state
print("clearing storage")
d.clear_storage_sync()
print("clear done")

# 2. Enroll a finger
template = FPrint.Print.new(d)

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

print("enrolling")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
p = d.enroll_sync(template, None, enroll_progress, None)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("enroll done")

# 3. List enrolled prints — should have exactly one
print("listing")
stored = d.list_prints_sync()
print("listing done")
assert len(stored) == 1

# 4. Verify against the enrolled print
print("verifying")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
verify_res, verify_print = d.verify_sync(p)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify done")
del p
assert verify_res == True

# 5. Identify (async) with deserialized prints
identified = False

def identify_done(dev, res):
    global identified
    identified = True
    identify_match, identify_print = dev.identify_finish(res)
    print('identification done: ', identify_match, identify_print)
    assert identify_match.equal(identify_print)

deserialized_prints = []
for p in stored:
    deserialized_prints.append(FPrint.Print.deserialize(p.serialize()))
    assert deserialized_prints[-1].equal(p)
del stored

print("async identifying")
d.identify(deserialized_prints, callback=identify_done)
del deserialized_prints

while not identified:
    ctx.iteration(True)

# 6. Delete the enrolled print
print("deleting")
d.delete_print_sync(p)
print("delete done")

d.close_sync()

del d
del c
