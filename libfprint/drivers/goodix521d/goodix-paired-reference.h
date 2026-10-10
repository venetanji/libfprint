/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bounded GF5298 paired-reference acquisition. Included after the device
 * definition. This path never decodes a press image or calls the NBIS path. */
#pragma once
#include "goodix-profile.h"


enum {
    PAIRED_REFERENCE_F1_PHASE0,
    PAIRED_REFERENCE_F1_PHASE1,
    PAIRED_REFERENCE_IMAGE1_PREPARE,
    PAIRED_REFERENCE_IMAGE1,
    PAIRED_REFERENCE_IMAGE1_RESTORE,
    PAIRED_REFERENCE_F2_PHASE0,
    PAIRED_REFERENCE_F2_PHASE1,
    PAIRED_REFERENCE_IMAGE2_PREPARE,
    PAIRED_REFERENCE_IMAGE2,
    PAIRED_REFERENCE_IMAGE2_RESTORE,
    PAIRED_REFERENCE_F3_PHASE0,
    PAIRED_REFERENCE_F3_PHASE1,
    PAIRED_REFERENCE_NUM_STATES,
};

static void paired_reference_log_failure(FpiSsm *ssm, GError *error, guint length)
{
    const gchar *domain = error->domain == FP_DEVICE_ERROR ? "fp-device-error-quark" :
                          error->domain == G_IO_ERROR ? "g-io-error-quark" : "other-domain-redacted";
    fp_warn("paired-reference-error stage=%u domain=%s domain_id=%u code=%d reply-length=%u",
            fpi_ssm_get_cur_state(ssm), domain, (guint)error->domain, error->code, length);
}

static void paired_reference_fail(FpiSsm *ssm, GError *error, guint length)
{
    paired_reference_log_failure(ssm,error,length);
    fpi_ssm_mark_failed(ssm,error);
}

static void paired_reference_reject(FpiSsm *ssm, GError *error, guint length)
{
    GoodixAcquisition *self=fpi_ssm_get_data(ssm);
    paired_reference_log_failure(ssm,error,length);
    /* Linux recovery policy: at most one re-acquisition in this same initialized
     * context and existing action deadline. Windows updates this seed after
     * validated stability rejection, but supplies no immediate retry count. */
    if (!acquisition_continue(self,ssm)) { g_error_free(error);return; }
    if (!self->paired_f1_valid || self->paired_retry_count) {
        fpi_ssm_mark_failed(ssm,error);return;
    }
    self->paired_retry_count++;
    for (guint i=0;i<8;i++) {
        guint16 sample=self->paired_fdt[0][2*i]|((guint16)self->paired_fdt[0][2*i+1]<<8);
        guint16 seed=(guint16)((sample>>1)*257u);
        self->paired_seed[2*i]=seed;self->paired_seed[2*i+1]=seed>>8;
    }
    self->paired_f1_valid=FALSE;
    memset(self->paired_fdt,0,sizeof self->paired_fdt);
    memset(self->paired_images,0,sizeof self->paired_images);
    g_error_free(error);
    fp_warn("paired-reference-retry attempt=2 max-attempts=2 seed=first-fdt scope=same-open");
    fpi_ssm_jump_to_state(ssm,PAIRED_REFERENCE_F1_PHASE0);
}

static guint paired_reference_fdt_max_delta(const guint8 a[16], const guint8 b[16])
{
    guint maximum = 0;
    for (guint i = 0; i < 8; i++) {
        gint av = (a[2*i] | ((guint16)a[2*i+1] << 8)) >> 1;
        gint bv = (b[2*i] | ((guint16)b[2*i+1] << 8)) >> 1;
        maximum = MAX(maximum, (guint)ABS(av - bv));
    }
    return maximum;
}

static gboolean paired_reference_fdt_stable(const guint8 a[16], const guint8 b[16], guint threshold)
{
    return paired_reference_fdt_max_delta(a, b) <= threshold;
}

static void paired_reference_command_done(FpDevice *dev, guint8 *data, guint16 length,
                                           gpointer user_data, GError *error)
{
    (void)dev; (void)data; (void)length;
    if (error) paired_reference_fail(user_data, error, length);
    else fpi_ssm_next_state(user_data);
}

static void paired_reference_register_done(FpDevice *dev, gpointer user_data, GError *error)
{
    FpiSsm *ssm = user_data;
    if (error) {
        paired_reference_fail(ssm, error, 0);
        return;
    }
    if (fpi_ssm_get_cur_state(ssm) == PAIRED_REFERENCE_IMAGE2_RESTORE) {
        GoodixAcquisition *self = fpi_ssm_get_data(ssm);
        GxFields fields;
        guint64 diagnostics[5];
        guint64 generation = (guint64)(guintptr)ssm;
        /* Restore 022c first, then reject an unstable image pair before F3.
         * c=b temporarily isolates the already known F1/F2 and image gates. */
        if (gx_fields(self->otp, 64, &fields) != GX_OK ||
            gx_background_gates(self->paired_fdt[0], self->paired_fdt[1], self->paired_fdt[1],
                                &self->paired_images[0], &self->paired_images[1],
                                fields.timing, generation, diagnostics) != GX_OK) {
            paired_reference_fail(ssm,
                g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "Invalid paired-reference image gate inputs"), 0);
            return;
        }
        if ((diagnostics[4] & 3) != 3) {
            paired_reference_reject(ssm,
                g_error_new_literal(FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,
                                    "Paired-reference image stability check failed"),0);
            return;
        }
    }
    fpi_ssm_next_state(ssm);
}

static void paired_reference_fdt_done(FpDevice *dev, guint8 *data, guint16 length,
                                       gpointer user_data, GError *error)
{
    FpiSsm *ssm = user_data;
    GoodixAcquisition *self = fpi_ssm_get_data(ssm);
    if (error) {
        paired_reference_fail(ssm, error, length);
        return;
    }
    /* The transport has already checked command ownership, ACK and checksum.
     * Opcode 36 data is IRQ:u16, touch flags:u16, then eight raw FDT words.
     * The manual Windows path does not treat nonzero touch flags as failure. */
    if (!data || length != 20) {
        paired_reference_fail(ssm,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Invalid paired-reference FDT reply length"), length);
        return;
    }
    guint state = fpi_ssm_get_cur_state(ssm);
    guint index = state == PAIRED_REFERENCE_F1_PHASE1 ? 1 :
                  state == PAIRED_REFERENCE_F2_PHASE1 ? 2 :
                  state == PAIRED_REFERENCE_F3_PHASE1 ? 3 : 0;
    if (!index) {
        paired_reference_fail(ssm,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Unexpected paired-reference FDT state"), length);
        return;
    }
    memcpy(self->paired_fdt[index - 1], data + 4, 16);
    GxFields fields;
    if (gx_fields(self->otp, 64, &fields) != GX_OK) {
        paired_reference_fail(ssm,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Invalid paired-reference factory fields"), length);
        return;
    }
    if (index == 1) self->paired_f1_valid=TRUE;
    if (index > 1 && !paired_reference_fdt_stable(self->paired_fdt[index - 2],
                                                self->paired_fdt[index - 1], fields.timing)) {
        guint delta = paired_reference_fdt_max_delta(self->paired_fdt[index - 2],
                                                      self->paired_fdt[index - 1]);
        fp_warn("paired-reference-fdt pair=%u/%u max-delta=%u threshold=%u",
                index - 1, index, delta, fields.timing);
        paired_reference_reject(ssm,
            g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                        "Paired-reference FDT stability check failed: pair=%u/%u max-delta=%u threshold=%u; keep reader clear during open",
                        index - 1, index, delta, fields.timing), length);
        return;
    }
    fpi_ssm_next_state(ssm);
}

static void paired_reference_image_done(FpDevice *dev, guint8 *data, guint16 length,
                                         gpointer user_data, GError *error)
{
    GoodixAcquisition *self = fpi_ssm_get_data(user_data);
    if (error) {
        paired_reference_fail(user_data, error, length);
        return;
    }
    /* The existing TLS image bridge validates the complete plaintext and CRC.
     * Decode separately for the pair gate; export the unchanged plaintext. */
    if (!data || length != GX_BLOB_SIZE) {
        paired_reference_fail(user_data,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Invalid paired-reference image length"), length);
        return;
    }
    const guint8 prefix[5] = {0};
    guint64 generation = (guint64)(guintptr)user_data;
    guint state = fpi_ssm_get_cur_state(user_data);
    guint index = state == PAIRED_REFERENCE_IMAGE1 ? 0 : 1;
    if ((state != PAIRED_REFERENCE_IMAGE1 && state != PAIRED_REFERENCE_IMAGE2) ||
        gx_image_blob(data, length, prefix, generation, generation, &self->paired_images[index]) != GX_OK) {
        paired_reference_fail(user_data,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Invalid paired-reference image integrity"), length);
        return;
    }
    fpi_ssm_next_state(user_data);
}

static void paired_reference_run(FpiSsm *ssm, FpDevice *dev)
{
    GoodixAcquisition *self = fpi_ssm_get_data(ssm);
    if (!acquisition_continue(self,ssm)) return;
    GxFields fields;
    GxSubmission submission;
    guint state = fpi_ssm_get_cur_state(ssm);
    /* Fresh construction starts with zero FdtBase. All three reads reuse one
     * seed; only the bounded same-open recovery replaces it between attempts. */
    if (gx_fields(self->otp, 64, &fields) != GX_OK ||
        gx_submission(state, fields.fdt, self->paired_seed, sizeof self->paired_seed, &submission) != GX_OK) {
        paired_reference_fail(ssm,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Invalid paired-reference command state"), 0);
        return;
    }
    switch (submission.opcode) {
    case 0x36: {
        gboolean reply = submission.payload[26] == 1;
        /* Phase 0 waits for ACK only; phase 1 waits for ACK then command data.
         * Keep the existing transport's ownership, ACK and checksum checks. */
        goodix_send_protocol(dev, submission.opcode, submission.payload,
                             submission.payload_len, NULL, TRUE, GOODIX_TIMEOUT, reply,
                             reply ? paired_reference_fdt_done : paired_reference_command_done, ssm);
        break;
    }
    case 0x80: {
        guint16 value = submission.payload[3] | ((guint16)submission.payload[4] << 8);
        goodix_send_write_sensor_register(dev, 0x022c, value,
                                          paired_reference_register_done, ssm);
        break;
    }
    case 0x20:
        goodix_tls_read_image(dev, submission.payload, 10, paired_reference_image_done, ssm);
        break;
    default:
        paired_reference_fail(ssm,
            g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                "Unsupported paired-reference command"), 0);
        break;
    }
}
