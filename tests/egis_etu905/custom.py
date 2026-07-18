#!/usr/bin/python3
import traceback

import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib, Gio

# Exit with error on any exception, included those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == "egis_etu905"
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)

d.open_sync()

def enroll_progress(*args):
    assert d.get_finger_status() == FPrint.FingerStatusFlags.PRESENT
    print("enroll progress stage: " +  str(args[1]))

def identify_done(dev, res):
    global identified
    identified = True
    identify_match, identify_print = dev.identify_finish(res)
    print("MATCH FOUND!" if identify_match else "NO MATCH FOUND")
    assert identify_match.equal(identify_print)

# List
print("--- LISTING ---")
stored = d.list_prints_sync()
prints1 = len(stored)
print(f"--- LIST DONE: Found {prints1} prints before enroll---")

# Enroll
print("--- ENROLLING ---")
template = FPrint.Print.new(d)

assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
p = d.enroll_sync(template, None, enroll_progress, None)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("--- ENROLL DONE ---")
del template

# List
print("--- LISTING ---")
stored = d.list_prints_sync()
prints2 = len(stored)
print(f"--- LIST DONE: Found {prints2} prints after enroll---")
assert (prints2 - prints1) == 1

# Cancel test - start async identify and cancel it
print("--- TESTING CANCELLATION ---")
deserialized_prints = []
for sp in stored:
    deserialized_prints.append(FPrint.Print.deserialize(sp.serialize()))

cancellable = Gio.Cancellable()
identify_cancelled = False
cancel_result = None

def identify_cancelled_cb(dev, res):
    global identify_cancelled, cancel_result
    identify_cancelled = True
    try:
        result = dev.identify_finish(res)
        cancel_result = result
    except Exception as e:
        cancel_result = e
        print(f"Identify cancelled with error: {e}")

d.identify(deserialized_prints, cancellable=cancellable, callback=identify_cancelled_cb)
cancellable.cancel()

while not identify_cancelled:
    ctx.iteration(True)
print(f"--- CANCELLATION TEST DONE, result: {cancel_result} ---")
del deserialized_prints

# Verify
print("--- VERIFYING ---")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
verify_res, verify_print = d.verify_sync(p)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print(f"--- VERIFY DONE: Result {verify_res} ---")

# Identify
print("--- ASYNC IDENTIFYING ---")
identified = False
deserialized_prints = []
for p in stored:
    deserialized_prints.append(FPrint.Print.deserialize(p.serialize()))
    assert deserialized_prints[-1].equal(p)

d.identify(deserialized_prints, callback=identify_done)
del deserialized_prints

while not identified:
    ctx.iteration(True)
print("--- IDENTIFY DONE ---")

# Delete
print("--- DELETING ---")
p_to_delete = next((sp for sp in stored if sp.equal(p)), None)
if p_to_delete:
    d.delete_print_sync(p_to_delete)
    print("--- DELETE DONE ---")
del p_to_delete
del p

# List
print("--- LISTING ---")
stored = d.list_prints_sync()
prints3 = len(stored)
print(f"--- LIST DONE: Found {prints3} prints after deleting---")
assert (prints2 - prints3) == 1
del stored

d.close_sync()

del d
del c
