/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "goodix-input.h"
#include "goodix-profile.h"
#include "goodix-config-base.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

gboolean
goodix521d_read_private (const char *path, guint8 *out, gsize length, GError **error)
{
  struct stat before, after;
  int fd = -1;
  gsize have = 0;
  if (!path || !g_path_is_absolute (path) || !out || !length || length > 1024)
    goto invalid;
  fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0 || fstat (fd, &before) || !S_ISREG (before.st_mode) ||
      before.st_size != (off_t) length || (before.st_mode & 0077) ||
      (before.st_uid != 0 && before.st_uid != geteuid ()))
    goto invalid;
  while (have < length)
    {
      ssize_t count = read (fd, out + have, length - have);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        goto invalid;
      have += count;
    }
  if (fstat (fd, &after) || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    goto invalid;
  close (fd);
  return TRUE;
invalid:
  if (fd >= 0)
    close (fd);
  if (out && length <= 1024)
    memset (out, 0, length);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Goodix provisioned input must be a stable private regular file with the expected length and owner");
  return FALSE;
}

gboolean
goodix521d_configure (const guint8 factory[64], guint8 config[256], GError **error)
{
  guint8 crc = 0;
  GxFields fields;
  if (!factory || !config)
    goto invalid;
  for (guint i = 0; i < 64; i++)
    {
      if (i == 30) continue;
      crc ^= factory[i];
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc << 1) ^ ((crc & 0x80) ? 7 : 0);
    }
  if ((guint8) (crc ^ 255) != factory[30] ||
      gx_fields (factory, 64, &fields) != GX_OK || gx_conservative_policy (&fields) ||
      gx_config (goodix521d_config_base, sizeof goodix521d_config_base,
                 GX_PINNED_TEMPLATE_SHA, &fields, config) != GX_OK)
    goto invalid;
  return TRUE;
invalid:
  if (config) memset (config, 0, 256);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Unsupported or invalid GF5298 factory profile");
  return FALSE;
}
