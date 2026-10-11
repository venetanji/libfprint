#!/usr/bin/env python3
"""Extract unchanged transport/driver bodies for the composed close regression."""
from pathlib import Path
import sys

transport, driver, output = map(Path, sys.argv[1:])
s = transport.read_text()
d = driver.read_text()

def segment(source, path, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return '#line %d "%s"\n%s\n' % (source.count('\n', 0, begin) + 1, path, source[begin:finish])

out = '#ifdef GOODIX_COMPOSED_TYPES\n'
out += segment(s, transport, 'typedef struct {\n    GoodixTlsServer*', 'G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE')
out += '#else\n'
for start, end in (
    ('static GSource *goodix_add_timeout', '// TODO remove every GDestroyNotify'),
    ('void goodix_reset_state', 'gboolean goodix_dev_deinit'),
    ('static void goodix_fdt_up_done', '/* A reverse event invalidates'),
    ('void goodix_receive_data_cb', 'void goodix_start_read_loop'),
    ('typedef struct {\n    FpDevice *dev;\n    GoodixNoneCallback callback;', '// ---- GOODIX RECEIVE SECTION END ----'),
    ('void goodix_send_protocol(', 'void goodix_send_nop'),
    ('static void candidate_tls_finish', 'static void on_goodix_tls_server_ready'),
    ('void goodix_cancel', '// ---- TLS SECTION END ----'),
    ('gboolean goodix_dev_init', 'void goodix_reset_state'),
    ('gboolean goodix_dev_deinit', '// ---- DEV SECTION END ----'),
):
    out += segment(s, transport, start, end)
out += '#define FpiDeviceGoodix521d ComposedDriver\n#define FP_DEVICE(d) ((d)->device)\n'
out += segment(d, driver, 'static void\nclear_release_guard', 'static gboolean\nreference_health_matches')
out += '#undef FP_DEVICE\n#undef FpiDeviceGoodix521d\n#endif\n'
output.write_text(out)
