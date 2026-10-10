#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read-only fprintd prompt monitor. Run before fprintd-verify or fprintd-enroll in another terminal.

Property names follow https://fprint.freedesktop.org/fprintd-dev/Device.html .
No Claim, capture, enrollment, authentication or service-control calls are made.
Observation is best effort: rapid transitions may be missed; an unobserved HOLD
never authorizes a REMOVE prompt.
"""
import argparse
import sys

NAME = 'Goodix GF5298 (experimental free host matching)'
SERVICE = 'net.reactivated.Fprint'
INTERFACE = SERVICE + '.Device'
KEYS = ('finger-needed', 'finger-present')

class PromptState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.previous = None
        self.held = False

    invalidate = reset

    def update(self, needed, present, initial=False):
        if type(needed) is not bool or type(present) is not bool:
            self.reset()
            raise ValueError('Finger properties must be booleans')
        pair = (needed, present)
        previous = self.previous
        self.previous = pair
        if initial:
            self.held = False
            return None
        if pair == previous:
            return None
        if needed and present:
            self.held = True
            return 'HOLD your finger still.'
        if needed:
            self.held = False
            return 'PLACE your finger on the reader.'
        if present and self.held and previous == (True, True):
            self.held = False
            return 'REMOVE your finger from the reader.'
        self.held = False
        if not present:
            return 'KEEP READER EMPTY until PLACE appears.'
        return None

def timeout_value(text):
    value = int(text)
    if not 1 <= value <= 900:
        raise ValueError('timeout must be between 1 and 900 seconds')
    return value

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='/net/reactivated/Fprint/Device/0')
    parser.add_argument('--timeout', type=timeout_value, default=900)
    args = parser.parse_args(argv)
    if not args.device.startswith('/net/reactivated/Fprint/Device/'):
        parser.error('use an exact fprintd device object path')
    try:
        import gi
        gi.require_version('Gio', '2.0')
        from gi.repository import Gio, GLib
    except ImportError:
        print('Python GObject is required (Arch package: python-gobject).', file=sys.stderr)
        return 2
    loop = GLib.MainLoop()
    state = PromptState()
    subscriptions = []
    status = [0]
    ready = [False]
    values = {}
    connection = None
    def stop(message, code=1):
        state.reset()
        ready[0] = False
        status[0] = code
        print(message, flush=True)
        loop.quit()
    def call(destination, path, interface, method, parameters, signature):
        return connection.call_sync(destination, path, interface, method, parameters,
                                    GLib.VariantType.new(signature),
                                    Gio.DBusCallFlags.NO_AUTO_START, 5000, None).unpack()
    def changed(conn, sender, path, interface, signal, parameters, *unused):
        if not ready[0]:
            return
        iface, changes, invalid = parameters.unpack()
        if iface != INTERFACE:
            return
        if any(key in invalid for key in KEYS) or 'name' in invalid:
            stop('Monitor stopped: device properties were invalidated. Restart the monitor.')
            return
        if 'name' in changes and changes['name'] != NAME:
            stop('Monitor stopped: device identity changed.')
            return
        try:
            # Read a coherent current snapshot rather than combining delayed
            # partial signals with the bootstrap snapshot.
            current = call(owner, args.device, 'org.freedesktop.DBus.Properties',
                           'GetAll', GLib.Variant('(s)', (INTERFACE,)), '(a{sv})')[0]
            if current.get('name') != NAME:
                raise ValueError('Device identity changed')
            values.update(current)
            prompt = state.update(current.get(KEYS[0]), current.get(KEYS[1]))
        except (GLib.Error, ValueError):
            stop('Monitor stopped: invalid finger-status property types.')
            return
        if prompt:
            print(prompt, flush=True)
    def owner_changed(conn, sender, path, interface, signal, parameters, *unused):
        name, old, new = parameters.unpack()
        if name == SERVICE and old != new:
            stop('Monitor stopped: fprintd exited or changed owner.')
    def closed(*unused):
        stop('Monitor stopped: system bus disconnected.')
    timer = None
    try:
        connection = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        connection.connect('closed', closed)
        owner = call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                     'org.freedesktop.DBus', 'GetNameOwner',
                     GLib.Variant('(s)', (SERVICE,)), '(s)')[0]
        devices = call(owner, '/net/reactivated/Fprint/Manager', SERVICE + '.Manager',
                       'GetDevices', None, '(ao)')[0]
        if args.device not in devices:
            raise ValueError('requested device is not listed by fprintd')
        subscriptions.append(connection.signal_subscribe(
            'org.freedesktop.DBus', 'org.freedesktop.DBus', 'NameOwnerChanged',
            '/org/freedesktop/DBus', SERVICE, Gio.DBusSignalFlags.NONE, owner_changed))
        subscriptions.append(connection.signal_subscribe(
            owner, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            args.device, INTERFACE, Gio.DBusSignalFlags.NONE, changed))
        # Flush subscription setup and discard bootstrap notifications; the final
        # read below establishes a fresh baseline and never authorizes REMOVE.
        connection.flush_sync(None)
        context = GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
        values.update(call(owner, args.device, 'org.freedesktop.DBus.Properties',
                           'GetAll', GLib.Variant('(s)', (INTERFACE,)), '(a{sv})')[0])
        if values.get('name') != NAME:
            raise ValueError('requested device is not the expected Goodix521d driver')
        state.update(values.get(KEYS[0]), values.get(KEYS[1]), initial=True)
        current_owner = call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                             'org.freedesktop.DBus', 'GetNameOwner',
                             GLib.Variant('(s)', (SERVICE,)), '(s)')[0]
        if owner != current_owner or status[0]:
            raise ValueError('fprintd changed during monitor setup')
        ready[0] = True
        print('Monitor connected. KEEP READER EMPTY. Start ONE normal-user fprintd-verify (or enroll only if needed) in another terminal; wait for PLACE, then follow HOLD and REMOVE here.', flush=True)
        def timed_out():
            nonlocal timer
            timer = None
            stop('Monitor timeout reached.', 0)
            return False
        timer = GLib.timeout_add_seconds(args.timeout, timed_out)
        loop.run()
    except KeyboardInterrupt:
        state.reset()
        print('\nMonitor stopped.', flush=True)
        return 0
    except (GLib.Error, ValueError) as error:
        print('Monitor unavailable: ' + str(error), file=sys.stderr)
        return 2
    finally:
        if timer:
            GLib.source_remove(timer)
        if connection:
            for subscription in subscriptions:
                connection.signal_unsubscribe(subscription)
    return status[0]

if __name__ == '__main__':
    sys.exit(main())
