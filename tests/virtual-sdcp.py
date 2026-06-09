#!/usr/bin/env python3

import sys
try:
    import gi
    import os

    from gi.repository import GLib

    import unittest
except Exception as e:
    print("Missing dependencies: %s" % str(e))
    sys.exit(77)

FPrint = None

# Only permit loading virtual_sdcp driver for tests in this file
os.environ['FP_DRIVERS_WHITELIST'] = 'virtual_sdcp'

if hasattr(os.environ, 'MESON_SOURCE_ROOT'):
    root = os.environ['MESON_SOURCE_ROOT']
else:
    root = os.path.join(os.path.dirname(__file__), '..')

ctx = GLib.main_context_default()

class VirtualSDCPBase(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        os.environ['FP_VIRTUAL_SDCP'] = '1'

        cls.ctx = FPrint.Context()

        cls.dev = None
        for dev in cls.ctx.get_devices():
            cls.dev = dev
            break

        assert cls.dev is not None, "You need to compile with virtual_sdcp for testing"

    @classmethod
    def tearDownClass(cls):
        del cls.dev
        del cls.ctx

    def setUp(self):
        self.ctx = FPrint.Context()
        self.assertIsNotNone(self.dev)
        self.assertFalse(self.dev.is_open())
        self.dev.open_sync()
        self.assertTrue(self.dev.is_open())

    def tearDown(self):
        self.dev.close_sync()
        self.assertFalse(self.dev.is_open())
        del self.ctx

class VirtualSDCP(VirtualSDCPBase):

    def setUp(self):
        os.environ.pop('FP_VIRTUAL_SDCP_NO_RECONNECT', None)
        super().setUp()

    def test_connect(self):
        # Nothing to do here since setUp and tearDown will open and close the device
        pass

    def test_reconnect(self):
        # Ensure device was opened once before, this may be a reconnect if
        # it is the same process as another test.
        self.dev.close_sync()

        # Check that a reconnect happens on next open. To know about this, we
        # need to parse check log messages for that.
        success = [False]
        def log_func(domain, level, msg):
            print("log: '%s', '%s', '%s'" % (str(domain), str(level), msg))
            if msg == 'SDCP Reconnect succeeded':
                success[0] = True

            # Call default handler
            GLib.log_default_handler(domain, level, msg)

        handler_id = GLib.log_set_handler('libfprint-sdcp_device', GLib.LogLevelFlags.LEVEL_DEBUG, log_func)
        self.dev.open_sync()
        GLib.log_remove_handler('libfprint-sdcp_device', handler_id)
        assert success[0]

    def test_list(self):
        prints = self.dev.list_prints_sync()
        assert len(prints) >= 0

    def test_enroll_list_verify(self):
        # Set up a new print
        template = FPrint.Print.new(self.dev)
        template.set_finger(FPrint.Finger.LEFT_THUMB)

        # Enroll the new print
        new_print = self.dev.enroll_sync(template, None, None, None)
        self.assertIsInstance(new_print, FPrint.Print)

        # Get the print list again and ensure there is exactly 1 print
        prints = self.dev.list_prints_sync()
        self.assertTrue(len(prints) == 1)

        # Ensure the one print from list is the same as new_print
        self.assertTrue(prints[0].equal(new_print))

        # Verify new_print
        verify_res, verify_print = self.dev.verify_sync(prints[0])
        self.assertTrue(verify_res)
        self.assertTrue(verify_print.equal(prints[0]))

        # Set up a second new print
        template = FPrint.Print.new(self.dev)
        template.set_finger(FPrint.Finger.LEFT_INDEX)

        # Enroll the second print
        new_print2 = self.dev.enroll_sync(template, None, None, None)
        self.assertIsInstance(new_print2, FPrint.Print)

        # Get the print list again and ensure there is exactly 2 prints
        prints = self.dev.list_prints_sync()
        self.assertTrue(len(prints) == 2)

        # Ensure the second print from list is the same as new_print2
        self.assertTrue(prints[1].equal(new_print2))

class VirtualSDCPNoReconnect(VirtualSDCPBase):

    def setUp(self):
        os.environ['FP_VIRTUAL_SDCP_NO_RECONNECT'] = '1'
        super().setUp()

    def test_connect(self):
        # Nothing to do here since setUp and tearDown will open and close the device
        pass

    def test_reconnect(self):
        # Ensure device was opened once before, this may be a reconnect if
        # it is the same process as another test.
        self.dev.close_sync()

        # Check that a reconnect happens on next open. To know about this, we
        # need to parse check log messages for that.
        success = [False]
        def log_func(domain, level, msg):
            print("log: '%s', '%s', '%s'" % (str(domain), str(level), msg))
            if msg == 'SDCP Reconnect succeeded':
                success[0] = True

            # Call default handler
            GLib.log_default_handler(domain, level, msg)

        handler_id = GLib.log_set_handler('libfprint-sdcp_device', GLib.LogLevelFlags.LEVEL_DEBUG, log_func)
        self.dev.open_sync()
        GLib.log_remove_handler('libfprint-sdcp_device', handler_id)

        # Ensure that we did NOT see "SDCP Reconnect succeeded" in the log
        assert success[0] == False


if __name__ == '__main__':
    try:
        gi.require_version('FPrint', '2.0')
        from gi.repository import FPrint
    except Exception as e:
        print("Missing dependencies: %s" % str(e))
        sys.exit(77)

    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
