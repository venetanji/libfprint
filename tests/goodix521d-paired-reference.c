/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Compile the actual paired SSM against an offline transport boundary. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include "goodix-profile.h"


typedef struct {
    guint8 *otp;
    guint8 candidate_config[256], candidate_image_request[10];
    guint8 paired_fdt[3][16];
    GxImage paired_images[2];
} GoodixAcquisition;
typedef GoodixAcquisition FpDevice;
typedef GoodixAcquisition FpImageDevice;
typedef struct { guint state; FpDevice *dev; gboolean done; } FpiSsm;
typedef void (*Reply)(FpDevice *, guint8 *, guint16, gpointer, GError *);
typedef void (*NoneReply)(FpDevice *, gpointer, GError *);
#define FPI_DEVICE_GOODIXTLS52XD(dev) (dev)
#define FP_IMAGE_DEVICE(dev) (dev)
#define FP_DEVICE_ERROR G_IO_ERROR
#define FP_DEVICE_ERROR_PROTO G_IO_ERROR_INVALID_DATA
#define FP_DEVICE_ERROR_NOT_SUPPORTED G_IO_ERROR_NOT_SUPPORTED
#define GOODIX_TIMEOUT 1000
#define fp_warn(...) g_test_message(__VA_ARGS__)

static GError *session_error;
static guint submissions, completed;
static gint fail_state;
static guint8 fdt_reply[32];
static guint16 fdt_reply_length;
static guint8 raw[2][7684];
static gint unstable_fdt;
static guint16 image_length;
static gchar *directory;
static const guint8 opcodes[12] = {0x36,0x36,0x80,0x20,0x80,0x36,0x36,0x80,0x20,0x80,0x36,0x36};
static const guint16 expected_words[4] = {303,295,305,297};

static gboolean acquisition_continue(GoodixAcquisition *self,FpiSsm *ssm) { (void)self;return !ssm->done; }
static guint fpi_ssm_get_cur_state(FpiSsm *ssm) { return ssm->state; }
static void fpi_ssm_next_state(FpiSsm *ssm);
static void fpi_ssm_mark_failed(FpiSsm *ssm, GError *error);
static gpointer fpi_ssm_get_data(FpiSsm *ssm) { return ssm->dev; }
static void paired_reference_complete(FpiSsm *ssm, FpDevice *dev, GError *error) { (void)ssm; (void)dev; g_assert_null(session_error); session_error=error; }
static void goodix_send_protocol(FpDevice *, guint8, guint8 *, guint16,
                                GDestroyNotify, gboolean, guint, gboolean, Reply, gpointer);
static void goodix_send_write_sensor_register(FpDevice *, guint16, guint16, NoneReply, gpointer);
static void goodix_tls_read_image(FpDevice *, gpointer, guint16, Reply, gpointer);

#include "goodix-paired-reference.h"

static void fpi_ssm_next_state(FpiSsm *ssm)
{
    g_assert_false(ssm->done);
    if (++ssm->state == PAIRED_REFERENCE_NUM_STATES) {
        ssm->done = TRUE; completed++;
        paired_reference_complete(ssm, ssm->dev, NULL);
    } else paired_reference_run(ssm, ssm->dev);
}
static void fpi_ssm_mark_failed(FpiSsm *ssm, GError *error)
{
    g_assert_false(ssm->done); g_assert_nonnull(error);
    ssm->done = TRUE;
    paired_reference_complete(ssm, ssm->dev, error);
}
static GError *injected(FpiSsm *ssm)
{
    return (gint)ssm->state == fail_state ?
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "synthetic timeout") : NULL;
}
static void note(FpiSsm *ssm, guint8 opcode)
{
    g_assert_false(ssm->done);
    g_assert_cmpuint(ssm->state, <, 12);
    g_assert_cmpuint(opcode, ==, opcodes[ssm->state]);
    g_assert_cmpuint(submissions++, ==, ssm->state);
}
static void words_match(const guint8 *payload)
{
    for (guint i=0; i<4; i++)
        g_assert_cmpuint(payload[2+2*i] | ((guint16)payload[3+2*i] << 8), ==, expected_words[i]);
}
static void goodix_send_protocol(FpDevice *dev, guint8 opcode, guint8 *payload, guint16 length,
                                GDestroyNotify free_func, gboolean checksum, guint timeout,
                                gboolean reply, Reply callback, gpointer data)
{
    FpiSsm *ssm = data; note(ssm, opcode);
    g_assert_null(free_func); g_assert_true(checksum);
    g_assert_cmpuint(timeout, ==, GOODIX_TIMEOUT);
    g_assert_cmpuint(opcode, ==, 0x36); g_assert_cmpuint(length, ==, 27);
    g_assert_cmpuint(payload[0], ==, ssm->state == 5 || ssm->state == 6 ? 0x8d : 0x0d);
    g_assert_cmpuint(payload[1], ==, 1); words_match(payload);
    const guint8 zero[16] = {0}; g_assert_cmpmem(payload+10, 16, zero, 16);
    gboolean phase1 = ssm->state == 1 || ssm->state == 6 || ssm->state == 11;
    g_assert_cmpuint(payload[26], ==, phase1); g_assert_cmpint(reply, ==, phase1);
    guint8 response[32]; memcpy(response, fdt_reply, sizeof response);
    if ((gint)ssm->state == unstable_fdt) { response[4] = 30; response[5] = 0; }
    callback(dev, phase1 ? response : NULL, phase1 ? fdt_reply_length : 0, data, injected(ssm));
}
static void goodix_send_write_sensor_register(FpDevice *dev, guint16 address, guint16 value,
                                             NoneReply callback, gpointer data)
{
    FpiSsm *ssm = data; note(ssm, 0x80);
    g_assert_cmpuint(address, ==, 0x022c);
    g_assert_cmpuint(value, ==, ssm->state == 4 || ssm->state == 9 ? 0x020a : 0x030a);
    callback(dev, data, injected(ssm));
}
static void goodix_tls_read_image(FpDevice *dev, gpointer request, guint16 length, Reply callback, gpointer data)
{
    FpiSsm *ssm = data; guint8 *payload = request; note(ssm, 0x20);
    g_assert_cmpuint(length, ==, 10);
    g_assert_cmpuint(payload[0], ==, ssm->state == 3 ? 0x01 : 0x81);
    g_assert_cmpuint(payload[1], ==, 3); words_match(payload);
    callback(dev, raw[ssm->state == 3 ? 0 : 1], image_length, data, injected(ssm));
}

static void setup(void)
{
    GError *error = NULL;
    directory = g_dir_make_tmp("goodix-paired-ssm-XXXXXX", &error);
    g_assert_no_error(error);
    g_setenv("GOODIX_RAW_EXPORT_DIR", directory, TRUE);
    submissions = completed = 0; fail_state = unstable_fdt = -1;
    image_length = 7684;
    memset(raw, 0, sizeof raw);
    memcpy(raw[0]+7680, "\xb1\x49\xc0\xf0", 4);
    memcpy(raw[1]+7680, "\xb1\x49\xc0\xf0", 4);
    memset(fdt_reply, 0, sizeof fdt_reply);
    fdt_reply[0] = 2; fdt_reply[2] = 255; fdt_reply_length = 20;
}
static void teardown(void)
{
    GDir *dir = g_dir_open(directory, 0, NULL); const char *name;
    while ((name = g_dir_read_name(dir))) {
        g_autofree gchar *path = g_build_filename(directory, name, NULL);
        g_assert_cmpint(unlink(path), ==, 0);
    }
    g_dir_close(dir); g_assert_cmpint(rmdir(directory), ==, 0);
    g_clear_pointer(&directory, g_free); g_clear_error(&session_error);
    g_unsetenv("GOODIX_RAW_EXPORT_DIR");
}
static void run(void)
{
    guint8 otp[64] = {0};
    otp[23]=0x40; otp[26]=37; otp[33]=47;
    otp[40]=7; otp[41]=39; otp[42]=49; otp[43]=41;
    otp[44]=97; otp[45]=101; otp[46]=103;
    FpDevice dev = {.otp=otp};
    FpiSsm ssm = {.dev=&dev}; paired_reference_run(&ssm, &dev);
    g_assert_true(ssm.done);
}
static void ack_then_data_phase(void)
{
    setup(); fail_state=1; run();
    g_assert_cmpuint(submissions, ==, 2); g_assert_cmpuint(completed, ==, 0);
    g_assert_error(session_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    teardown();
}
#define ISOLATE() do { if (!g_test_subprocess()) { \
    g_test_trap_subprocess(NULL, 0, 0); \
    g_test_trap_assert_passed(); return; } } while (0)

static void complete_pair(void)
{
    ISOLATE(); setup(); run();
    g_assert_cmpuint(submissions, ==, 12); g_assert_cmpuint(completed, ==, 1);
    g_assert_no_error(session_error);
    teardown();
}
static void unstable_f2_stops_before_image2(void)
{
    ISOLATE(); setup(); unstable_fdt=6; run();
    g_assert_cmpuint(submissions, ==, 7); g_assert_cmpuint(completed, ==, 0);
    g_assert_error(session_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    teardown();
}
static void unstable_f3_rejects_completion(void)
{
    ISOLATE(); setup(); unstable_fdt=11; run();
    g_assert_cmpuint(submissions, ==, 12); g_assert_cmpuint(completed, ==, 0);
    g_assert_error(session_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    teardown();
}
static void all_command_errors_stop(void)
{
    ISOLATE();
    for (gint state=0; state<12; state++) {
        setup(); fail_state=state; run();
        g_assert_cmpuint(submissions, ==, state+1); g_assert_cmpuint(completed, ==, 0);
        g_assert_error(session_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
        teardown();
    }
}
static void malformed_fdt_stops(void)
{
    for (guint size=0; size<=21; size++) {
        if (size==20) continue;
        setup(); fdt_reply_length=size; run();
        g_assert_cmpuint(submissions, ==, 2); g_assert_cmpuint(completed, ==, 0);
        g_assert_error(session_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
        teardown();
    }
}
static void malformed_image_stops(void)
{
    setup(); image_length=7683; run();
    g_assert_cmpuint(submissions, ==, 4); g_assert_cmpuint(completed, ==, 0);
    g_assert_error(session_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    teardown();
}

static void frame_with_native_pixels(const guint16 native[5120], guint8 out[7684])
{
    for (guint j=0; j<1280; j++) {
        guint16 p[4];
        for (guint k=0; k<4; k++) {
            guint i=j*4+k; p[k]=native[(i%64)*80+i/64];
        }
        guint8 *b=out+j*6;
        b[0]=(p[0]>>8)|((p[1]&15)<<4); b[1]=p[0]; b[2]=p[2];
        b[3]=p[1]>>4; b[4]=p[3]>>4; b[5]=(p[2]>>8)|((p[3]&15)<<4);
    }
    guint32 crc=0xffffffff;
    for (guint i=0; i<7680; i++) {
        crc^=(guint32)out[i]<<24;
        for (guint bit=0; bit<8; bit++) crc=(crc<<1)^((crc&0x80000000u)?0x04c11db7u:0);
    }
    out[7680]=crc>>8; out[7681]=crc; out[7682]=crc>>24; out[7683]=crc>>16;
}
static void pair_mad_boundary(void)
{
    ISOLATE();
    for (guint difference=199; difference<=200; difference++) {
        setup(); guint16 a[5120], b[5120];
        for (guint i=0; i<5120; i++) { a[i]=1000; b[i]=1000+difference; }
        frame_with_native_pixels(a, raw[0]); frame_with_native_pixels(b, raw[1]); run();
        g_assert_cmpuint(completed, ==, difference==199 ? 1 : 0);
        g_assert_cmpuint(submissions, ==, difference==199 ? 12 : 10);
        teardown();
    }
}
static void pair_excludes_native_border(void)
{
    ISOLATE(); setup(); guint16 a[5120], b[5120];
    for (guint y=0; y<64; y++) for (guint x=0; x<80; x++) {
        a[y*80+x]=1000;
        b[y*80+x]=(y<2||y>=62||x<2||x>=78)?4095:1199;
    }
    frame_with_native_pixels(a, raw[0]); frame_with_native_pixels(b, raw[1]); run();
    g_assert_cmpuint(submissions, ==, 12); g_assert_cmpuint(completed, ==, 1);
    teardown();
}
static void corrupt_image_stops(void)
{
    setup(); raw[0][7680]^=1; run();
    g_assert_cmpuint(submissions, ==, 4); g_assert_cmpuint(completed, ==, 0);
    g_assert_error(session_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO); teardown();
}
static void fdt_shifted_threshold(void)
{
    guint8 a[16]={0}, b[16]={0};
    b[14]=17; g_assert_true(paired_reference_fdt_stable(a,b,8));
    b[14]=18; g_assert_false(paired_reference_fdt_stable(a,b,8));
    b[14]=1; g_assert_true(paired_reference_fdt_stable(a,b,0));
    b[14]=2; g_assert_false(paired_reference_fdt_stable(a,b,0));
}

static void unexpected_fdt_state_stops(void)
{
    setup(); FpDevice dev={0}; FpiSsm ssm={.dev=&dev,.state=3};
    paired_reference_fdt_done(&dev, fdt_reply, 20, &ssm, NULL);
    g_assert_true(ssm.done); g_assert_cmpuint(submissions, ==, 0);
    g_assert_error(session_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    teardown();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/paired-reference/ack-then-data-phase", ack_then_data_phase);
    g_test_add_func("/paired-reference/complete-pair", complete_pair);
    g_test_add_func("/paired-reference/unstable-f2", unstable_f2_stops_before_image2);
    g_test_add_func("/paired-reference/unstable-f3", unstable_f3_rejects_completion);
    g_test_add_func("/paired-reference/all-command-errors", all_command_errors_stop);
    g_test_add_func("/paired-reference/malformed-fdt", malformed_fdt_stops);
    g_test_add_func("/paired-reference/malformed-image", malformed_image_stops);
    g_test_add_func("/paired-reference/pair-mad-boundary", pair_mad_boundary);
    g_test_add_func("/paired-reference/pair-native-crop", pair_excludes_native_border);
    g_test_add_func("/paired-reference/corrupt-image", corrupt_image_stops);
    g_test_add_func("/paired-reference/fdt-shifted-threshold", fdt_shifted_threshold);
    g_test_add_func("/paired-reference/unexpected-fdt-state", unexpected_fdt_state_stops);
    return g_test_run();
}
