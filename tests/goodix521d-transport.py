#!/usr/bin/env python3
"""Run source-executed Goodix receiver and authenticated TLS regressions.

Needs Python 3, a C compiler, POSIX threads, GLib/GIO and OpenSSL development files.
Only small GLib/libfprint surface shims are used; parser and TLS production
functions, OpenSSL ciphers and peer handshakes execute unchanged.
All peer keys and packet contents are synthetic; no USB device is opened.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
SOURCE = ROOT / 'libfprint/drivers/goodix521d/transport'


def segment(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def run(command):
    subprocess.run([str(part) for part in command], check=True)


def main():
    source = (SOURCE / 'goodix.c').read_text()
    header = (SOURCE / 'goodix.h').read_text()
    protocol = (SOURCE / 'goodix_proto.c').read_text()
    assert 'FpImageDevice' not in header.replace('no FpImageDevice activation', '')
    assert 'FP_TYPE_IMAGE_DEVICE' not in source
    assert 'fpi_image_device_activate_complete' not in source
    assert 'goodix_send_preset_psk_write' not in source
    assert 'goodix_read_owner_current' in source
    assert 'g_cancellable_reset(priv->transfer_cancel_tkn)' not in source
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
             *shlex.split(os.environ.get('CFLAGS', ''))]
    linker_flags = shlex.split(os.environ.get('LDFLAGS', ''))
    with tempfile.TemporaryDirectory(prefix='goodix521d-transport-') as name:
        build = Path(name)
        receiver = '\n'.join([
            segment(protocol, 'guint8 goodix_calc_checksum', 'void goodix_encode_pack'),
            protocol[protocol.index('gboolean goodix_decode_pack'):],
            segment(source, 'void goodix_reset_state', 'gboolean goodix_dev_deinit'),
            segment(source, 'static void goodix_fdt_up_done', 'void goodix_receive_none'),
            segment(source, 'static void goodix_receive_ack_full', 'void goodix_receive_data_cb'),
        ])
        (build / 'receiver.inc').write_text(receiver)
        run([*compiler, *flags, '-Wno-unused-parameter', '-Wno-unused-function',
             '-I' + str(build), TESTS / 'goodix521d-transport-receiver.c',
             *linker_flags, '-o', build / 'receiver'])
        run([build / 'receiver'])
        run([*compiler, *flags, '-Wno-unused-parameter', '-Wno-unused-function',
             '-Wno-address', '-I' + str(build), TESTS / 'goodix521d-transport-fdt.c',
             *linker_flags, '-o', build / 'fdt'])
        run([build / 'fdt'])
        common = [*compiler, *flags, '-pthread',
                  '-I' + str(TESTS / 'goodix521d-transport-shim'),
                  '-I' + str(SOURCE), '-I' + str(build)]
        timeout = (segment(source, 'static GSource *goodix_add_timeout',
                           'static void goodix_stop_read_loop') +
                   segment(source, 'void goodix_read_tls', 'enum tls_states'))
        (build / 'timeout.inc').write_text(timeout)
        run([*common, TESTS / 'goodix521d-transport-timeout.c',
             *linker_flags, '-o', build / 'timeout'])
        run([build / 'timeout'])
        completion = (segment(source, 'static void candidate_tls_finish',
                              'static void on_goodix_tls_server_ready') +
                      segment(source, 'void goodix_tls_set_psk',
                              'gboolean goodix_shutdown_tls'))
        (build / 'completion.inc').write_text(completion)
        run([*common, TESTS / 'goodix521d-transport-completion.c',
             '-lssl', '-lcrypto', *linker_flags, '-o', build / 'completion'])
        run([build / 'completion'])
        if 'GOODIX_TEST_GLIB_CFLAGS' in os.environ:
            glib_cflags = shlex.split(os.environ['GOODIX_TEST_GLIB_CFLAGS'])
            glib_libs = shlex.split(os.environ['GOODIX_TEST_GLIB_LIBS'])
        else:
            glib_cflags = shlex.split(subprocess.check_output(
                ['pkg-config', '--cflags', 'gio-2.0'], text=True))
            glib_libs = shlex.split(subprocess.check_output(
                ['pkg-config', '--libs', 'gio-2.0'], text=True))
        real_glib = [*compiler, *flags, '-Wno-unused-parameter',
                     '-Wno-unused-function', '-Wno-unused-variable', '-Wno-address', *glib_cflags,
                     '-I' + str(SOURCE), '-I' + str(build)]
        health = ''
        if 'gboolean goodix_transport_health(' in source:
            health += segment(source, 'static gboolean goodix_fdt_up_current(', 'static void goodix_fdt_up_maybe_complete')
            health += segment(source, 'static gboolean goodix_transport_health_full(', '/* End transport health */')
            tls_source = (SOURCE / 'goodixtls.c').read_text()
            health = segment(tls_source, 'gboolean goodix_tls_server_health(', '/* End TLS health */') + health
            health += segment(source, 'void goodix_send_reset(', 'void goodix_send_firmware_version')
        (build / 'health.inc').write_text(health)
        run([*real_glib, TESTS / 'goodix521d-transport-health.c',
             *glib_libs, *linker_flags, '-o', build / 'health'])
        run([build / 'health'])
        stop_loop = segment(source, 'static void goodix_stop_read_loop',
                            '// TODO remove every GDestroyNotify')
        lifecycle = (segment(source, 'typedef struct {\n    FpDevice *dev;',
                             'G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE') + stop_loop +
                     segment(source, 'void goodix_receive_data_cb',
                             'void goodix_receive_timeout_cb') +
                     segment(source, 'void goodix_start_read_loop',
                             '// ---- GOODIX RECEIVE SECTION END ----'))
        (build / 'lifecycle.inc').write_text(lifecycle)
        cancellation = (stop_loop +
                        segment(source, 'void goodix_reset_state', 'gboolean goodix_dev_deinit') +
                        segment(source, 'static void goodix_fdt_up_done', 'void goodix_receive_none') +
                        segment(source, 'static void candidate_tls_finish',
                                'static void on_goodix_tls_server_ready') +
                        segment(source, 'enum goodix_tls_handshake_stages',
                                'static void do_tls_handshake') +
                        segment(source, 'void goodix_cancel', '// ---- TLS SECTION END ----'))
        (build / 'cancel.inc').write_text(cancellation)
        opening = stop_loop + segment(source, 'gboolean goodix_dev_init',
                                      '// ---- DEV SECTION END ----')
        (build / 'open.inc').write_text(opening)
        (build / 'send.inc').write_text(segment(source, 'gboolean goodix_send_data',
                                               'gboolean goodix_send_pack'))
        (build / 'psk-reply.inc').write_text(segment(
            source, 'void goodix_receive_preset_psk_read',
            'void goodix_receive_firmware_version'))
        register = (segment(source, 'static GSource *goodix_add_timeout',
                            'static void goodix_stop_read_loop') +
                    segment(source, 'void goodix_reset_state', 'gboolean goodix_dev_deinit') +
                    segment(source, 'static void goodix_fdt_up_done', 'void goodix_receive_default') +
                    segment(source, 'static void goodix_receive_ack_full', 'void goodix_receive_data_cb') +
                    segment(source, 'void goodix_receive_timeout_cb', 'void goodix_start_read_loop') +
                    segment(source, 'gboolean goodix_send_pack', 'void goodix_send_nop') +
                    segment(source, 'void goodix_send_write_sensor_register', 'void goodix_send_read_sensor_register'))
        (build / 'register.inc').write_text(register)
        run([*real_glib, SOURCE / 'goodix_proto.c',
             TESTS / 'goodix521d-transport-register.c',
             *glib_libs, *linker_flags, '-o', build / 'register'])
        run([build / 'register'])
        for case in ('lifecycle', 'cancel', 'open', 'send', 'psk-reply'):
            run([*real_glib, TESTS / ('goodix521d-transport-' + case + '.c'),
                 *glib_libs, '-lcrypto', *linker_flags, '-o', build / case])
            run([build / case])
        run([*common, SOURCE / 'goodixtls.c', TESTS / 'goodix521d-transport-key.c',
             *['-Wl,--wrap=' + name for name in ('open', 'fstat', 'read', 'close')],
             '-lssl', '-lcrypto', *linker_flags, '-o', build / 'key'])
        run([build / 'key'])
        run([*common, SOURCE / 'goodixtls.c', TESTS / 'goodix521d-transport-tls.c',
             *['-Wl,--wrap=' + name for name in (
                 'socketpair', 'pthread_create', 'send', 'SSL_read',
                 'SSL_CTX_new', 'SSL_new', 'SSL_set_fd')],
             '-lssl', '-lcrypto', *linker_flags, '-o', build / 'tls'])
        for case in ('unsupported-cipher', 'authenticated', 'wrong-key',
                     'unauthenticated', 'handshake-timeout', 'blocked-read',
                     'blocked-send', 'init-failure', 'early-cleanup',
                     'peer-disconnect', 'tampered-plaintext', 'recv-timeout',
                     'cleanup-retains'):
            run([build / 'tls', case])
    print('PASS: Goodix transport source contracts and portable regressions', flush=True)


if __name__ == '__main__':
    main()
