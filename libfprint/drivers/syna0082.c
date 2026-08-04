/*
 * Experimental Synaptics/PQI 06cb:0082 image driver.
 *
 * Device-specific initialization blobs are intentionally provisioned outside
 * the source tree. Set LIBFPRINT_SYNA0082_BLOB_DIR to the directory containing
 * config-39.bin, config-06.bin, and scan-matrix-02.bin.
 */

#define FP_COMPONENT "syna0082"

#include "drivers_api.h"

#define EP_OUT 0x01
#define EP_IN 0x81
#define EP_INTERRUPT 0x83
#define USB_TIMEOUT 5000

#define CONFIG_39_LENGTH 125
#define CONFIG_06_LENGTH 10501
#define SCAN_02_LENGTH 18869
#define SCAN_RESPONSE_LENGTH 2154
#define IMAGE_RESPONSE_LENGTH 8082
#define IMAGE_HEADER_LENGTH 18
#define IMAGE_WIDTH 56
#define IMAGE_HEIGHT 144

typedef struct _FpiDeviceSyna0082 FpiDeviceSyna0082;

struct _FpiDeviceSyna0082
{
  FpImageDevice parent;

  guint8        *config_39;
  guint8        *config_06;
  guint8        *scan_02;
  gsize          init_stage;
  gsize          rearm_stage;
  guint          capture_count;
  gboolean       deactivating;
};

G_DECLARE_FINAL_TYPE (FpiDeviceSyna0082, fpi_device_syna0082,
                      FPI, DEVICE_SYNA0082, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceSyna0082, fpi_device_syna0082,
               FP_TYPE_IMAGE_DEVICE);

static const guint8 command_1a[] = { 0x1a };
static const guint8 command_01[] = { 0x01 };
static const guint8 command_19[] = { 0x19 };
static const guint8 command_75[] = { 0x75 };
static const guint8 command_51[] = { 0x51, 0x00, 0x20, 0x00, 0x00 };

static void start_interrupt (FpiDeviceSyna0082 *self);

static guint16
read_le16 (const guint8 *data)
{
  return data[0] | ((guint16) data[1] << 8);
}

static guint32
read_le32 (const guint8 *data)
{
  return data[0] | ((guint32) data[1] << 8) |
         ((guint32) data[2] << 16) | ((guint32) data[3] << 24);
}

static gboolean
load_blob (const gchar  *directory,
           const gchar  *name,
           gsize         expected_length,
           guint8        expected_command,
           guint8      **contents,
           GError      **error)
{
  g_autofree gchar *path = g_build_filename (directory, name, NULL);
  gsize length = 0;

  if (!g_file_get_contents (path, (gchar **) contents, &length, error))
    return FALSE;
  if (length != expected_length || (*contents)[0] != expected_command)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "%s has invalid framing (length=%" G_GSIZE_FORMAT ")",
                   path,
                   length);
      g_clear_pointer (contents, g_free);
      return FALSE;
    }
  return TRUE;
}

static void
complete_deactivation (FpiDeviceSyna0082 *self)
{
  if (!self->deactivating)
    return;
  self->deactivating = FALSE;
  fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (self), NULL);
}

static void
fail_session_or_deactivate (FpiDeviceSyna0082 *self,
                            GError             *error)
{
  if (self->deactivating)
    {
      g_clear_error (&error);
      complete_deactivation (self);
    }
  else
    fpi_image_device_session_error (FP_IMAGE_DEVICE (self), error);
}

static void
capture_read_cb (FpiUsbTransfer *transfer,
                 FpDevice       *device,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);
  FpImage *image;
  guint16 width;
  guint16 height;

  if (error)
    {
      fail_session_or_deactivate (self, error);
      return;
    }
  if (transfer->actual_length != IMAGE_RESPONSE_LENGTH ||
      read_le16 (transfer->buffer) != 0 ||
      read_le32 (transfer->buffer + 2) != IMAGE_RESPONSE_LENGTH - 6)
    {
      fail_session_or_deactivate (
        self,
        g_error_new_literal (G_IO_ERROR,
                             G_IO_ERROR_INVALID_DATA,
                             "Invalid 06cb:0082 image frame"));
      return;
    }

  width = read_le16 (transfer->buffer + 6);
  height = read_le16 (transfer->buffer + 8);
  if (width != IMAGE_WIDTH || height != IMAGE_HEIGHT)
    {
      fail_session_or_deactivate (
        self,
        g_error_new (G_IO_ERROR,
                     G_IO_ERROR_INVALID_DATA,
                     "Unexpected 06cb:0082 image size %ux%u",
                     width,
                     height));
      return;
    }

  image = fp_image_new (width, height);
  memcpy (image->data,
          transfer->buffer + IMAGE_HEADER_LENGTH,
          width * height);
  image->flags |= FPI_IMAGE_PARTIAL;
  self->capture_count++;
  fpi_image_device_image_captured (FP_IMAGE_DEVICE (self), image);
  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (self), FALSE);
}

static void
capture_write_cb (FpiUsbTransfer *transfer,
                  FpDevice       *device,
                  gpointer        user_data,
                  GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);

  if (error)
    fail_session_or_deactivate (self, error);
}

static void
start_capture (FpiDeviceSyna0082 *self)
{
  FpDevice *device = FP_DEVICE (self);
  FpiUsbTransfer *read_transfer;
  FpiUsbTransfer *write_transfer;

  read_transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, IMAGE_RESPONSE_LENGTH);
  read_transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (read_transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           capture_read_cb,
                           NULL);

  write_transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (write_transfer,
                                   EP_OUT,
                                   (guint8 *) command_51,
                                   sizeof (command_51),
                                   NULL);
  write_transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (write_transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           capture_write_cb,
                           NULL);
}

static void
interrupt_cb (FpiUsbTransfer *transfer,
              FpDevice       *device,
              gpointer        user_data,
              GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);

  if (error)
    {
      fail_session_or_deactivate (self, error);
      return;
    }
  if (transfer->actual_length == 5 && transfer->buffer[0] == 0x03 &&
      (transfer->buffer[1] == 0x42 || transfer->buffer[1] == 0x43) &&
      transfer->buffer[2] == 0x04)
    {
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (self), TRUE);
      start_capture (self);
    }
  else
    start_interrupt (self);
}

static void
start_interrupt (FpiDeviceSyna0082 *self)
{
  FpDevice *device = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  if (self->deactivating)
    {
      complete_deactivation (self);
      return;
    }

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_interrupt (transfer, EP_INTERRUPT, 5);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer,
                           0,
                           fpi_device_get_cancellable (device),
                           interrupt_cb,
                           NULL);
}

static void init_send_stage (FpiDeviceSyna0082 *self);

static void
init_read_cb (FpiUsbTransfer *transfer,
              FpDevice       *device,
              gpointer        user_data,
              GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);

  if (error)
    {
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (self), error);
      return;
    }
  self->init_stage++;
  if (self->init_stage == 7)
    {
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (self), NULL);
    }
  else
    init_send_stage (self);
}

static void
init_write_cb (FpiUsbTransfer *transfer,
               FpDevice       *device,
               gpointer        user_data,
               GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);
  static const gsize response_lengths[] = {
    2, 38, 68, 10, 2, 2, SCAN_RESPONSE_LENGTH
  };
  FpiUsbTransfer *read_transfer;

  if (error)
    {
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (self), error);
      return;
    }

  read_transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (read_transfer,
                              EP_IN,
                              response_lengths[self->init_stage]);
  read_transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (read_transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           init_read_cb,
                           NULL);
}

static void
init_send_stage (FpiDeviceSyna0082 *self)
{
  static const guint8 *static_requests[] = {
    command_1a, command_01, command_19, command_75
  };
  static const gsize static_lengths[] = {
    sizeof (command_1a), sizeof (command_01),
    sizeof (command_19), sizeof (command_75)
  };
  FpDevice *device = FP_DEVICE (self);
  FpiUsbTransfer *transfer;
  guint8 *request;
  gsize request_length;

  if (self->init_stage < 4)
    {
      request = (guint8 *) static_requests[self->init_stage];
      request_length = static_lengths[self->init_stage];
    }
  else if (self->init_stage == 4)
    {
      request = self->config_39;
      request_length = CONFIG_39_LENGTH;
    }
  else if (self->init_stage == 5)
    {
      request = self->config_06;
      request_length = CONFIG_06_LENGTH;
    }
  else
    {
      request = self->scan_02;
      request_length = SCAN_02_LENGTH;
    }

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (transfer,
                                   EP_OUT,
                                   request,
                                   request_length,
                                   NULL);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           init_write_cb,
                           NULL);
}

static void
operation_control_cb (FpiUsbTransfer *transfer,
                      FpDevice       *device,
                      gpointer        user_data,
                      GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);

  if (error)
    {
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (self), error);
      return;
    }
  if (transfer->actual_length != 2 || transfer->buffer[0] != 0 ||
      transfer->buffer[1] != 0)
    {
      fpi_image_device_activate_complete (
        FP_IMAGE_DEVICE (self),
        g_error_new_literal (G_IO_ERROR,
                             G_IO_ERROR_INVALID_DATA,
                             "Invalid 06cb:0082 operation-control response"));
      return;
    }
  init_send_stage (self);
}

static void
dev_activate (FpImageDevice *image_device)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (image_device);
  FpiUsbTransfer *transfer;

  self->deactivating = FALSE;
  self->init_stage = 0;
  self->capture_count = 0;
  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x14,
                                 0,
                                 0,
                                 2);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           operation_control_cb,
                           NULL);
}

static void rearm_send_stage (FpiDeviceSyna0082 *self);

static void
rearm_read_cb (FpiUsbTransfer *transfer,
               FpDevice       *device,
               gpointer        user_data,
               GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);

  if (error)
    {
      fail_session_or_deactivate (self, error);
      return;
    }
  self->rearm_stage++;
  if (self->rearm_stage == 2)
    start_interrupt (self);
  else
    rearm_send_stage (self);
}

static void
rearm_write_cb (FpiUsbTransfer *transfer,
                FpDevice       *device,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (device);
  FpiUsbTransfer *read_transfer;
  gsize response_length;

  if (error)
    {
      fail_session_or_deactivate (self, error);
      return;
    }
  response_length = self->rearm_stage == 0 ? 2 : SCAN_RESPONSE_LENGTH;
  read_transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, response_length);
  read_transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (read_transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           rearm_read_cb,
                           NULL);
}

static void
rearm_send_stage (FpiDeviceSyna0082 *self)
{
  FpDevice *device = FP_DEVICE (self);
  FpiUsbTransfer *transfer;
  guint8 *request;
  gsize request_length;

  if (self->rearm_stage == 0)
    {
      request = self->config_39;
      request_length = CONFIG_39_LENGTH;
    }
  else
    {
      /* Windows uses 0x23 for subsequent enrollment acquisitions. */
      self->scan_02[1749] = 0x23;
      request = self->scan_02;
      request_length = SCAN_02_LENGTH;
    }

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (transfer,
                                   EP_OUT,
                                   request,
                                   request_length,
                                   NULL);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer,
                           USB_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           rearm_write_cb,
                           NULL);
}

static void
dev_change_state (FpImageDevice      *image_device,
                  FpiImageDeviceState state)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (image_device);

  if (state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    return;
  if (self->capture_count == 0)
    start_interrupt (self);
  else
    {
      self->rearm_stage = 0;
      rearm_send_stage (self);
    }
}

static void
dev_deactivate (FpImageDevice *image_device)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (image_device);

  self->deactivating = TRUE;
  complete_deactivation (self);
}

static void
dev_open (FpImageDevice *image_device)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (image_device);
  FpDevice *device = FP_DEVICE (self);
  const gchar *directory;
  GError *error = NULL;

  directory = g_getenv ("LIBFPRINT_SYNA0082_BLOB_DIR");
  if (directory == NULL || *directory == '\0')
    directory = "/var/lib/libfprint/syna0082";

  if (!load_blob (directory, "config-39.bin", CONFIG_39_LENGTH, 0x39,
                  &self->config_39, &error) ||
      !load_blob (directory, "config-06.bin", CONFIG_06_LENGTH, 0x06,
                  &self->config_06, &error) ||
      !load_blob (directory, "scan-matrix-02.bin", SCAN_02_LENGTH, 0x02,
                  &self->scan_02, &error))
    {
      fpi_image_device_open_complete (image_device, error);
      return;
    }

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device),
                                     0, 0, &error))
    {
      fpi_image_device_open_complete (image_device, error);
      return;
    }
  fpi_image_device_open_complete (image_device, NULL);
}

static void
dev_close (FpImageDevice *image_device)
{
  FpiDeviceSyna0082 *self = FPI_DEVICE_SYNA0082 (image_device);
  GError *error = NULL;

  g_usb_device_release_interface (
    fpi_device_get_usb_device (FP_DEVICE (self)), 0, 0, &error);
  g_clear_pointer (&self->config_39, g_free);
  g_clear_pointer (&self->config_06, g_free);
  g_clear_pointer (&self->scan_02, g_free);
  fpi_image_device_close_complete (image_device, error);
}

static const FpIdEntry id_table[] = {
  { .vid = 0x06cb, .pid = 0x0082 },
  { .vid = 0, .pid = 0 },
};

static void
fpi_device_syna0082_init (FpiDeviceSyna0082 *self)
{
}

static void
fpi_device_syna0082_class_init (FpiDeviceSyna0082Class *klass)
{
  FpDeviceClass *device_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *image_class = FP_IMAGE_DEVICE_CLASS (klass);

  device_class->id = "syna0082";
  device_class->full_name = "Synaptics/PQI 06cb:0082";
  device_class->type = FP_DEVICE_TYPE_USB;
  device_class->id_table = id_table;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;

  image_class->img_open = dev_open;
  image_class->img_close = dev_close;
  image_class->activate = dev_activate;
  image_class->change_state = dev_change_state;
  image_class->deactivate = dev_deactivate;
  image_class->bz3_threshold = 20;
  image_class->img_width = IMAGE_WIDTH;
  image_class->img_height = IMAGE_HEIGHT;
}
