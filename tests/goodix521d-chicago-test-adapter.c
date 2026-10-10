/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-only access to a stage boundary, never linked into libfprint. */
#include "../libfprint/drivers/goodix521d/chicago/goodix-chicago-preprocess.c"
void goodix_test_normalize (GoodixChicagoPreprocessor *self,
                            const guint16 *current, const guint16 *background,
                            gint purpose, guint16 *source, guint16 *auxiliary);

void goodix_test_normalize (GoodixChicagoPreprocessor *self,
                            const guint16 *current, const guint16 *background,
                            gint purpose, guint16 *source, guint16 *auxiliary)
{
  normalize_frame (self, current, background, purpose, source, auxiliary);
}
