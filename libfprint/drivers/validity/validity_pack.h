/*
 * Validity VCSFW driver — pack/unpack utilities
 *
 * Varargs helpers for packing bytes into buffers and unpacking bytes from
 * buffers.  Built on top of FpiByteWriter / FpiByteReader.
 *
 * Format codes (same for pack and unpack):
 *   'b'  uint8
 *   'h'  uint16  little-endian
 *   'H'  uint16  big-endian
 *   'w'  uint32  little-endian
 *   'W'  uint32  big-endian
 *   't'  uint24  big-endian   (TLS lengths)
 *   'x'  1 pad / skip byte
 *   'd'  data blob  (pack: const guint8 *, gsize)
 *                    (unpack: const guint8 **, gsize — pointer set to internal)
 *
 * Pack into caller-provided buffer — returns bytes written:
 *   gsize n = validity_pack (buf, sizeof buf, "bwwb", cmd, addr, val, len);
 *
 * Pack into newly allocated buffer — returns g_malloc'd pointer:
 *   guint8 *p = validity_pack_new (&out_len, "bhd", cmd, id, data, data_len);
 *
 * Unpack from buffer — returns TRUE on success:
 *   gboolean ok = validity_unpack (data, len, "hhxxh", &a, &b, &c);
 */

#pragma once

#include "fpi-byte-writer.h"   /* includes fpi-byte-reader.h */
#include <stdarg.h>

/* ---- internal: write one format char ---- */
static inline gboolean
validity_pack_one (FpiByteWriter *w,
                   char           code,
                   va_list       *ap)
{
  switch (code)
    {
    case 'b':
      return fpi_byte_writer_put_uint8 (w, (guint8) va_arg (*ap, int));

    case 'h':
      return fpi_byte_writer_put_uint16_le (w, (guint16) va_arg (*ap, int));

    case 'H':
      return fpi_byte_writer_put_uint16_be (w, (guint16) va_arg (*ap, int));

    case 'w':
      return fpi_byte_writer_put_uint32_le (w, va_arg (*ap, guint32));

    case 'W':
      return fpi_byte_writer_put_uint32_be (w, va_arg (*ap, guint32));

    case 't':
      return fpi_byte_writer_put_uint24_be (w, va_arg (*ap, guint32));

    case 'x':
      return fpi_byte_writer_put_uint8 (w, 0);

    case 'd':
      {
        const guint8 *d = va_arg (*ap, const guint8 *);
        gsize len = va_arg (*ap, gsize);
        if (len == 0)
          return TRUE;
        return fpi_byte_writer_put_data (w, d, len);
      }

    default:
      return FALSE;
    }
}

/* ---- internal: read one format char ---- */
static inline gboolean
validity_unpack_one (FpiByteReader *r,
                     char           code,
                     va_list       *ap)
{
  switch (code)
    {
    case 'b':
      return fpi_byte_reader_get_uint8 (r, va_arg (*ap, guint8 *));

    case 'h':
      return fpi_byte_reader_get_uint16_le (r, va_arg (*ap, guint16 *));

    case 'H':
      return fpi_byte_reader_get_uint16_be (r, va_arg (*ap, guint16 *));

    case 'w':
      return fpi_byte_reader_get_uint32_le (r, va_arg (*ap, guint32 *));

    case 'W':
      return fpi_byte_reader_get_uint32_be (r, va_arg (*ap, guint32 *));

    case 't':
      return fpi_byte_reader_get_uint24_be (r, va_arg (*ap, guint32 *));

    case 'x':
      return fpi_byte_reader_skip (r, 1);

    case 'd':
      {
        const guint8 **out = va_arg (*ap, const guint8 * *);
        gsize len = va_arg (*ap, gsize);
        return fpi_byte_reader_get_data (r, len, out);
      }

    default:
      return FALSE;
    }
}

/**
 * validity_pack:
 * @buf:      destination buffer (caller-provided)
 * @buf_size: size of @buf in bytes
 * @fmt:      format string (see header comment)
 * @...:      values matching each format code
 *
 * Packs fields into @buf according to @fmt.
 *
 * Returns: number of bytes written.
 */
G_GNUC_UNUSED static gsize
validity_pack (guint8     *buf,
               gsize       buf_size,
               const char *fmt,
               ...)
{
  FpiByteWriter w;
  va_list ap;

  fpi_byte_writer_init_with_data (&w, buf, buf_size, FALSE);
  va_start (ap, fmt);
  for (const char *p = fmt; *p; p++)
    validity_pack_one (&w, *p, &ap);
  va_end (ap);
  return fpi_byte_writer_get_pos (&w);
}

/**
 * validity_pack_new:
 * @out_len: (out): set to the number of bytes written
 * @fmt:     format string (see header comment)
 * @...:     values matching each format code
 *
 * Packs fields into a newly allocated buffer.
 *
 * Returns: (transfer full): a g_malloc'd buffer.  Free with g_free().
 */
G_GNUC_UNUSED static guint8 *
validity_pack_new (gsize      *out_len,
                   const char *fmt,
                   ...)
{
  FpiByteWriter w;
  va_list ap;

  /* Compute total size from format so we allocate exactly once. */
  gsize size = 0;

  va_start (ap, fmt);
  for (const char *p = fmt; *p; p++)
    {
      switch (*p)
        {
        case 'b':
          size += 1;
          (void) va_arg (ap, int);
          break;

        case 'x':
          size += 1;
          break;

        case 'h':
        case 'H':
          size += 2;
          (void) va_arg (ap, int);
          break;

        case 't':
          size += 3;
          (void) va_arg (ap, guint32);
          break;

        case 'w':
        case 'W':
          size += 4;
          (void) va_arg (ap, guint32);
          break;

        case 'd':
          (void) va_arg (ap, const guint8 *);
          size += va_arg (ap, gsize);
          break;

        default:
          break;
        }
    }
  va_end (ap);

  fpi_byte_writer_init_with_size (&w, size, FALSE);

  va_start (ap, fmt);
  for (const char *p = fmt; *p; p++)
    validity_pack_one (&w, *p, &ap);
  va_end (ap);

  *out_len = fpi_byte_writer_get_pos (&w);
  return fpi_byte_writer_reset_and_get_data (&w);
}

/**
 * validity_unpack:
 * @data:     source buffer
 * @data_len: length of @data
 * @fmt:      format string (see header comment)
 * @...:      pointers matching each format code
 *
 * Unpacks fields from @data according to @fmt.  Stops on the first
 * bounds error.
 *
 * Returns: %TRUE if every field was read, %FALSE on short data.
 */
G_GNUC_UNUSED static gboolean
validity_unpack (const guint8 *data,
                 gsize         data_len,
                 const char   *fmt,
                 ...)
{
  FpiByteReader r;
  va_list ap;

  fpi_byte_reader_init (&r, data, data_len);
  va_start (ap, fmt);
  for (const char *p = fmt; *p; p++)
    {
      if (!validity_unpack_one (&r, *p, &ap))
        {
          va_end (ap);
          return FALSE;
        }
    }
  va_end (ap);
  return TRUE;
}
