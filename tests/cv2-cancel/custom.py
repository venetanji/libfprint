#!/usr/bin/env python3

import sys
import traceback

import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib, Gio

sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == 'cv2'
d.open_sync()

cancellable = Gio.Cancellable()
stages = []
result = {}


def progress(dev, stage, pnt, data, error):
    print('enroll progress: stage %d, error %s' % (stage, error))
    if not error:
        stages.append(stage)


def on_finger_status(dev, pspec):
    # Cancel while the reader waits for the second press. This point is
    # deterministic, so the replay sees the same USB traffic.
    if (stages and not cancellable.is_cancelled() and
            dev.get_finger_status() & FPrint.FingerStatusFlags.NEEDED):
        print('canceling')
        cancellable.cancel()


def done(dev, res):
    try:
        dev.enroll_finish(res)
        result['error'] = None
    except GLib.Error as e:
        result['error'] = e


handler = d.connect('notify::finger-status', on_finger_status)

print('>>> Enroll RIGHT INDEX: press ONCE, then wait')
template = FPrint.Print.new(d)
template.set_finger(FPrint.Finger.RIGHT_INDEX)
d.enroll(template, cancellable=cancellable, progress_cb=progress, callback=done)

while 'error' not in result:
    ctx.iteration(True)

d.disconnect(handler)
assert result['error'] is not None
assert result['error'].matches(Gio.io_error_quark(), Gio.IOErrorEnum.CANCELLED), result['error']
assert stages == [1], stages
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE

print('>>> The reader is usable again: enroll RIGHT INDEX fully')
template = FPrint.Print.new(d)
template.set_finger(FPrint.Finger.RIGHT_INDEX)
p = d.enroll_sync(template, None, progress, None)

print('deleting the print')
d.delete_print_sync(p)

d.close_sync()

del d
del c
