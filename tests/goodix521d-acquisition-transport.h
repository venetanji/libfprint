/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real acquisition/FpiSsm + exact source-extracted transport and close bodies.
 * Only interface I/O and TLS shutdown are synthetic; no USB device is opened. */
#include "fpi-usb-transfer.h"
#include "../libfprint/drivers/goodix521d/transport/goodixtls.h"
#include "../libfprint/drivers/goodix521d/transport/goodix_proto.h"
#define GOODIX_COMPOSED_TYPES
#include "goodix521d-acquisition-transport-generated.h"
#undef GOODIX_COMPOSED_TYPES

static FpiDeviceGoodixTlsPrivate composed_state;
static FpiDeviceGoodixTlsClass composed_class;
static gboolean composed_mode, composed_shutdown_ok, composed_reenter_cancel;
static guint composed_callbacks, composed_destroys, composed_claims, composed_releases;
static guint composed_commands, composed_packets, composed_drained;
static EventWait composed_wait;
typedef struct {
  FpDevice *device;
  GoodixAcquisition *acquisition;
  gboolean transport_open, release_pending, contact_waiting;
  guint64 release_generation, waiting_action_id;
  GError *release_error;
} ComposedDriver;
static ComposedDriver *composed_driver;

#undef goodix_wait_fdt_up
#undef goodix_wait_fdt_down
#undef goodix_send_protocol
#define goodix_wait_fdt_up composed_wait_fdt_up
#define goodix_wait_fdt_down composed_wait_fdt_down
#define goodix_send_protocol composed_send_protocol
#define goodix_dev_init composed_dev_init
#define goodix_dev_deinit composed_dev_deinit
#define goodix_reset_state composed_reset_state
#define goodix_receive_done composed_receive_done
#define goodix_receive_data_cb composed_receive_data_cb
#define goodix_wait_read_idle composed_wait_read_idle
#define goodix_receive_timeout_cb composed_receive_timeout_cb
#define goodix_cancel composed_cancel
#define goodix_send_pack composed_send_pack
#define goodix_shutdown_tls composed_shutdown_tls
#define goodix_receive_pack composed_receive_pack
#define goodix_receive_data composed_receive_data
#define fpi_device_goodixtls_get_instance_private(d) ((void)(d), &composed_state)
#define FPI_DEVICE_GOODIXTLS_GET_CLASS(d) ((void)(d), &composed_class)
#define FPI_DEVICE_GOODIXTLS(d) ((FpiDeviceGoodixTls *)(d))
#define fpi_device_get_usb_device(d) ((GUsbDevice *)(d))
#define g_usb_device_claim_interface composed_claim_interface
#define g_usb_device_release_interface composed_release_interface

void composed_reset_state(FpDevice *);
void composed_wait_fdt_up(FpDevice *,guint,GoodixDefaultCallback,gpointer,GDestroyNotify);
void composed_wait_fdt_down(FpDevice *,guint,GoodixDefaultCallback,gpointer,GDestroyNotify);
void composed_send_protocol(FpDevice *,guint8,guint8 *,guint16,GDestroyNotify,gboolean,guint,gboolean,GoodixCmdCallback,gpointer);
gboolean composed_dev_init(FpDevice *,GError **);
gboolean composed_dev_deinit(FpDevice *,GError **);
void composed_receive_data_cb(FpiUsbTransfer *,FpDevice *,gpointer,GError *);
void composed_wait_read_idle(FpDevice *,guint,GoodixNoneCallback,gpointer);
void composed_receive_done(FpDevice *, guint8 *, guint16, GError *);
void composed_receive_timeout_cb(FpDevice *, gpointer);
void composed_cancel(FpDevice *);
static void candidate_diagnostic(FpDevice *d,GError *e,const char *s,FpiUsbTransfer *t)
{(void)d;(void)e;(void)s;(void)t;}
static gboolean composed_claim_interface(GUsbDevice *d,gint i,guint f,GError **e)
{(void)d;(void)i;(void)f;(void)e;composed_claims++;return TRUE;}
static gboolean composed_release_interface(GUsbDevice *d,gint i,guint f,GError **e)
{
  (void)d;(void)i;(void)f;(void)e;
  /* Interface release must not leave a timer, owner, SSM, or duplicate result. */
  g_assert_null(composed_state.fdt_up_callback);
  g_assert_null(composed_state.fdt_up_timeout);
  g_assert_null(capture->ssm);
  g_assert_cmpuint(contact.done,==,1);g_assert_cmpuint(contact.released,==,1);
  composed_releases++;return TRUE;
}
static gboolean composed_shutdown_tls(FpDevice *d,GError **e)
{
  (void)d;
  if(!composed_shutdown_ok)g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"synthetic TLS shutdown retained");
  return composed_shutdown_ok;
}
static void composed_receive_pack(FpDevice *d,guint8 *b,guint32 n)
{(void)d;(void)b;(void)n;composed_packets++;g_assert_not_reached();}
static void composed_receive_data(FpDevice *d)
{(void)d;g_assert_not_reached();}
static gboolean composed_send_pack(FpDevice *d,guint8 flags,guint8 *b,guint16 n,GDestroyNotify destroy,GError **e)
{
  (void)e;g_assert_cmpuint(flags,==,GOODIX_FLAGS_MSG_PROTOCOL);g_assert_cmpuint(n,>=,4);
  if(composed_state.cmd==0x34)contact.up_ack_mask|=1u<<(composed_state.fdt_up_phases_sent-1);
  if(destroy)destroy(b);
  composed_receive_done(d,NULL,0,NULL);
  return TRUE;
}
#include "goodix521d-acquisition-transport-generated.h"
#undef goodix_wait_fdt_up
#undef goodix_wait_fdt_down
#undef goodix_send_protocol
#undef goodix_dev_init
#undef goodix_dev_deinit
#undef goodix_reset_state
#undef goodix_receive_done
#undef goodix_receive_data_cb
#undef goodix_wait_read_idle
#undef goodix_receive_timeout_cb
#undef goodix_cancel
#undef goodix_send_pack
#undef goodix_shutdown_tls
#undef goodix_receive_pack
#undef goodix_receive_data
#undef fpi_device_goodixtls_get_instance_private
#undef FPI_DEVICE_GOODIXTLS_GET_CLASS
#undef FPI_DEVICE_GOODIXTLS
#undef fpi_device_get_usb_device
#undef g_usb_device_claim_interface
#undef g_usb_device_release_interface
#define goodix_wait_fdt_up mock_goodix_wait_fdt_up
#define goodix_wait_fdt_down mock_goodix_wait_fdt_down
#define goodix_send_protocol mock_goodix_send_protocol

static void composed_event(FpDevice *dev,guint8 *b,guint16 n,gpointer data,GError *error)
{
  (void)data;composed_callbacks++;
  if(error) {
    g_assert_nonnull(composed_driver);
    g_assert_false(composed_driver->release_pending);
    g_assert_false(composed_driver->contact_waiting);
    g_assert_cmpuint(composed_driver->release_generation,==,0);
    g_assert_cmpuint(composed_driver->waiting_action_id,==,0);
    g_assert_true(capture->cancelled);
  }
  memset(&contact.up_wait,0,sizeof contact.up_wait);
  composed_wait.callback(dev,b,n,composed_wait.data,error);
  if(composed_reenter_cancel)composed_cancel(dev);
}
static void composed_destroy(gpointer data)
{
  (void)data;composed_destroys++;
  composed_wait.destroy(composed_wait.data);
  memset(&composed_wait,0,sizeof composed_wait);
}
static void composed_register_up(FpDevice *dev,guint timeout)
{
  composed_wait=contact.up_wait;
  composed_wait_fdt_up(dev,timeout,composed_event,NULL,composed_destroy);
}
