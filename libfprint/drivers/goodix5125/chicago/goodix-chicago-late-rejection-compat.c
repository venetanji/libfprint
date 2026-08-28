// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Literal compatibility translation of AlgoChicago+0x2ce50 for template types
 * 7, 10, 23, 25, and 26. Keep this separate from the reviewed type-24 policy.
 * Refactor one mode at a time only alongside its complete matcher and official
 * differential-oracle coverage. */

#include "goodix-chicago-late-rejection-private.h"

static gboolean
match_signed_subtract_overflow (gint32 left,
                                gint32 right)
{
  const gint64 result = (gint64) left - right;

  return result < G_MININT32 || result > G_MAXINT32;
}

static gboolean
match_late_rejection_policy_compat (gint32 *param_1,
                                    gint32 *param_2,
                                    gint32  param_3,
                                    gint32 *param_4,
                                    gint32 *param_5,
                                    gint32 *param_6,
                                    gint32 *param_7)
{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  gboolean bVar6;
  int iVar7;
  int iVar8;
  int iVar9;
  int iVar10;
  int iVar11;
  int iVar12;
  int iVar13;
  int iVar14;
  int iVar15;
  int iVar16;
  int iVar17;
  int iVar18;
  gboolean bVar19;
  gboolean bVar20;
  gint32 inverse[6];

  iVar14 = param_1[1];
  iVar1 = *param_1;
  bVar6 = FALSE;
  iVar2 = param_1[0x43];
  iVar18 = param_5[1];
  iVar12 = param_5[2];
  if (param_5[2] < *param_5)
    iVar12 = *param_5;
  iVar17 = iVar1 * iVar14;
  iVar7 = goodix_chicago_match_transform_overlap_area_type24 (
    iVar1, iVar14, param_4);
  goodix_chicago_match_invert_transform_q8_type24 (param_4, inverse);
  iVar8 = goodix_chicago_match_transform_overlap_area_type24 (
    iVar1, iVar14, inverse);
  iVar1 = param_2[8];
  iVar3 = param_2[5];
  iVar14 = iVar3 + iVar1;
  if (iVar8 < iVar7)
    iVar8 = iVar7;
  if (4 < iVar12)
    {
      iVar7 = iVar8 * 100;
      if (iVar18 < 2)
        {
          if ((((((iVar7 <= iVar17 * 0x5c) || (0xd1 < iVar3)) || (0xd1 < iVar1)) &&
                (((iVar9 = iVar17 * 0x5f, iVar7 <= iVar9 || (0xd1 < iVar3)) || (0xdb < iVar1)))) &&
               (((iVar7 < iVar9 || (0xd9 < iVar3)) || (199 < iVar1)))) &&
              ((iVar7 <= iVar9 ||
                (((0xd3 < iVar3 || (0xd6 < iVar1)) &&
                  ((iVar7 <= iVar9 || ((199 < iVar3 || (0xe5 < iVar1))))))))))
            {
              if (((iVar7 <= iVar17 * 0x57) ||
                   ((((param_3 != 10 || (9 < *param_2)) || (10 < param_2[1])) ||
                     ((0xd3 < iVar3 || (0xcc < iVar1)))))) ||
                  (((0xe < param_2[0xb] || ((0x22 < param_2[10] || (0x59 < param_2[9])))) ||
                    (iVar7 = param_2[0x15], 0x40 < param_2[0x17] + iVar7))))
                goto LAB_18002d1fa;
              bVar20 = match_signed_subtract_overflow (iVar7, 0x22);
              iVar9 = iVar7 + -0x22;
              bVar19 = iVar7 == 0x22;
LAB_18002d1f3:
              if (!bVar19 && bVar20 == (iVar9 < 0))
                goto LAB_18002d1fa;
            }
        }
      else
        {
          if (iVar17 * 0x5a < iVar7)
            {
              if (iVar3 < 0xe1)
                {
                  if (iVar1 < 0xd2)
                    goto LAB_18002d1f5;
                  goto LAB_18002cf9f;
                }
            }
          else
            {
LAB_18002cf9f:
              if (((iVar3 < 0xe1) && (*param_2 < 7)) && (iVar1 < 0xd2))
                goto LAB_18002d1f5;
            }
          if (((((iVar7 <= iVar17 * 0x5f) || (iVar18 < 4)) && ((iVar7 <= iVar17 * 0x55 || (iVar18 < 5)))
               ) && (((iVar7 <= iVar17 * 0x5f || (0xf0 < iVar3)) || (0xf0 < iVar1)))) &&
              ((((iVar7 <= iVar17 * 0x5a || (0xeb < iVar3)) || (0xe6 < iVar1)) &&
                ((iVar7 <= iVar17 * 0x58 ||
                  ((((((param_3 != 10 || (0xd < *param_2)) ||
                       ((0x11 < param_2[1] || ((0xbd < iVar3 || (0xd8 < iVar1)))))) || (0x14 < param_2[0xb])
                     ) || (((0x22 < param_2[10] || (0x6d < param_2[9])) ||
                            (0x2d < param_2[0x17] + param_2[0x15])))) &&
                    ((((iVar7 <= iVar17 * 0x58 || (param_3 != 10)) ||
                       ((10 < *param_2 || ((0xc < param_2[1] || (199 < iVar3)))))) ||
                      ((0xd8 < iVar1 ||
                        ((((0x14 < param_2[0xb] || (0x2c < param_2[10])) || (0x72 < param_2[9])) ||
                          (0x1e < param_2[0x17] + param_2[0x15]))))))))))))))
            {
              if ((((iVar17 * 0x54 < iVar7) && (param_3 == 10)) &&
                   ((*param_2 < 0x10 && ((param_2[1] < 0x11 && (iVar3 < 0xc2)))))) &&
                  ((iVar1 < 0xcd &&
                    ((((param_2[0xb] < 0x19 && (param_2[10] < 0x37)) && (param_2[9] < 0x77)) &&
                      (iVar7 = param_2[0x15], param_2[0x17] + iVar7 < 0x62))))))
                {
                  bVar20 = match_signed_subtract_overflow (iVar7, 0x2d);
                  iVar9 = iVar7 + -0x2d;
                  bVar19 = iVar7 == 0x2d;
                  goto LAB_18002d1f3;
                }
              goto LAB_18002d1fa;
            }
        }
LAB_18002d1f5:
      bVar6 = TRUE;
    }
LAB_18002d1fa:
  if (3 < iVar12)
    {
      if (iVar18 < 2)
        {
          iVar7 = param_2[0x15];
          if (iVar7 < 0x5a)
            {
              iVar9 = *param_2;
              iVar11 = iVar8 * 100;
              if (iVar9 < 0x18)
                {
                  if (((((((iVar17 * 0x5f < iVar11) && (iVar14 < 0x19f)) ||
                          ((iVar16 = iVar17 * 0x5a, iVar16 < iVar11 && (iVar14 < 0x19a)))) ||
                         ((iVar17 * 0x55 < iVar11 && (iVar14 < 0x195)))) ||
                        ((iVar16 < iVar11 &&
                          (((iVar14 < 0x1a4 && (iVar7 < 0x32)) ||
                            ((iVar16 < iVar11 && ((iVar7 < 0x46 && (param_2[0xb] < 0x2d)))))))))) ||
                       ((iVar9 < 0xc && ((iVar17 * 0x50 < iVar11 && (iVar14 < 0x195)))))) ||
                      ((iVar9 < 0x10 &&
                        ((((iVar17 * 0x50 < iVar11 && (iVar14 < 0x177)) && (iVar7 < 0x33)) &&
                          (param_2[0xb] < 0x1f))))))
                    bVar6 = TRUE;
                  if (4 < *param_6)
                    {
                      if (iVar17 * 0x62 < iVar11)
                        bVar6 = TRUE;
                      if (((iVar17 * 0x5a < iVar11) && (iVar1 < 0xd2)) && (param_2[0xb] < 0x23))
                        bVar6 = TRUE;
                    }
                }
              else if (((iVar17 * 0x5f < iVar11) && (iVar14 < 0x195)) ||
                       (((iVar17 * 99 < iVar11 && ((iVar3 < 0xd0 && (iVar1 < 0xcf)))) &&
                         ((param_2[0xb] < 0x31 && ((iVar2 < 0x47 && (param_2[4] < 0xe2))))))))
                {
                  bVar6 = TRUE;
                }
            }
        }
      else if ((((iVar17 * 0x55 < iVar8 * 100) && (iVar3 < 0xdc)) && (iVar1 < 0xd3)) ||
               ((((iVar3 < 0xd2 && (iVar1 < 0xce)) && (5 < *param_6)) ||
                 (((iVar17 * 0x5f < iVar8 * 100 && (iVar3 < 0xe7)) && (iVar1 < 0xe7))))))
        {
          bVar6 = TRUE;
        }
    }
  if (2 < iVar12)
    {
      if (iVar18 < 2)
        {
          if ((param_2[0x15] < 0x5a) && (param_2[4] < 0xf1))
            {
              iVar7 = iVar8 * 100;
              if (*param_2 < 0x19)
                {
                  if ((((iVar17 * 0x5a < iVar7) && (iVar14 < 0x195)) ||
                       ((iVar17 * 0x5f < iVar7 &&
                         ((iVar14 < 0x19c ||
                           (((iVar17 * 0x5f < iVar7 && (iVar14 < 0x1a5)) && (param_2[0x15] < 0x46)))))))) ||
                      (((iVar17 * 0x61 < iVar7 && (iVar14 < 400)) && (param_2[0xb] < 0x24))))
                    bVar6 = TRUE;
                }
              else if (((iVar17 * 0x5f < iVar7) && (iVar14 < 400)) && (param_2[0xb] < 0x15))
                {
                  bVar6 = TRUE;
                }
            }
        }
      else if (((iVar17 * 0x5c < iVar8 * 100) && (iVar3 < 0xdc)) && (iVar1 < 0xce))
        {
          bVar6 = TRUE;
        }
    }
  if (1 < iVar12)
    {
      if (iVar18 < 2)
        {
          iVar7 = param_2[0x15];
          if ((iVar7 < 0x5a) && (param_2[4] < 0xf0))
            {
              iVar9 = *param_2;
              iVar11 = iVar8 * 100;
              if (iVar9 < 0x18)
                {
                  if ((((iVar17 * 0x5a < iVar11) &&
                        (((iVar14 < 0x196 && ((param_2[0xb] < 0x24 || (iVar9 < 0x12)))) ||
                          ((iVar17 * 0x5a < iVar11 &&
                            (((iVar1 < 0xd2 && (param_2[0xb] < 0x24)) && (iVar7 < 0x3c)))))))) ||
                       (((iVar17 * 0x5f < iVar11 && (iVar9 < 0x17)) &&
                         ((param_2[0xb] < 0x24 && (iVar7 < 0x3c)))))) ||
                      ((((iVar17 * 0x62 < iVar11 && (iVar9 < 0x17)) && (param_2[0xb] < 0x29)) &&
                        (iVar7 < 0x41))))
                    bVar6 = TRUE;
                }
              else
                {
                  if ((((iVar17 * 0x5f < iVar11) && (iVar3 < 0xd3)) && (iVar1 < 0xd3)) &&
                      ((param_2[0xb] < 0x28 && (iVar2 < 0x3d))))
                    bVar6 = TRUE;
                  if (((iVar17 * 0x62 < iVar11) && (iVar3 < 0xd3)) &&
                      ((param_2[0xb] < 0x29 && ((iVar2 < 0x49 && (param_2[4] < 0xe6))))))
                    bVar6 = TRUE;
                }
            }
        }
      else
        {
          iVar7 = iVar8 * 100;
          if ((((((((((iVar17 * 0x62 < iVar7) && (param_3 == 10)) && (*param_2 < 0xb)) &&
                    ((param_2[1] < 0xc && (iVar3 < 0xd2)))) &&
                   ((iVar1 < 0xec && ((param_2[9] < 0x3c && (param_2[0x17] + param_2[0x15] < 0x15)))))) ||
                  (((iVar17 * 0x60 < iVar7 &&
                     ((((((param_3 == 10 && (*param_2 < 0x10)) && (param_2[1] < 0x12)) &&
                         ((iVar3 < 0xc3 && (iVar1 < 0xdb)))) && (param_2[0xb] < 0x18)) &&
                       ((param_2[10] < 0x32 && (param_2[9] < 0x80)))))) &&
                    (param_2[0x17] + param_2[0x15] < 0x29)))) ||
                 ((iVar17 * 0x5c < iVar7 &&
                   (((((param_3 == 10 && (*param_2 < 0xd)) && (param_2[1] < 0xe)) &&
                      (((iVar3 < 0xd4 && (iVar1 < 0xe5)) &&
                        (((param_2[0xb] < 0x2b && ((param_2[10] < 0x35 && (param_2[9] < 0x55)))) &&
                          (param_2[0x17] + param_2[0x15] < 0x23)))))) ||
                     ((((iVar17 * 0x5c < iVar7 && (param_3 == 10)) && (*param_2 < 0xd)) &&
                       ((((param_2[1] < 0xd && (iVar3 < 0xd0)) &&
                          ((iVar1 < 0xdd && ((param_2[0xd] == 1 && (param_2[0xb] < 0x24)))))) &&
                         ((param_2[10] < 0x33 &&
                           ((param_2[9] < 0x5f && (param_2[0x17] + param_2[0x15] < 0x1f)))))))))))))) ||
                (((iVar17 * 0x5b < iVar7 &&
                   (((((((param_3 == 10 && (*param_2 < 10)) && (param_2[1] < 0xb)) &&
                        ((iVar3 < 0xd0 && (iVar1 < 0xe5)))) &&
                       ((param_2[0xb] < 0x2e && ((param_2[10] < 0x3d && (param_2[9] < 0x4b)))))) &&
                      (param_2[0x17] + param_2[0x15] < 0x23)) ||
                     (((((iVar17 * 0x5b < iVar7 && (param_3 == 10)) && (*param_2 < 0xd)) &&
                        (((param_2[1] < 0x10 && (iVar3 < 0xd0)) &&
                          ((iVar1 < 0xd5 && ((param_2[0xb] < 0x2e && (param_2[10] < 0x2e)))))))) &&
                       ((param_2[9] < 0x55 && (param_2[0x17] + param_2[0x15] < 0x1f)))))))) ||
                  ((((((iVar17 * 0x5a < iVar7 && (param_3 == 10)) && (*param_2 < 0xe)) &&
                      ((param_2[1] < 0xf && (iVar3 < 0xd0)))) &&
                     (((iVar1 < 0xd7 && ((param_2[0xb] < 0x1d && (param_2[10] < 0x25)))) &&
                       (param_2[9] < 0x5a)))) && (param_2[0x17] + param_2[0x15] < 0x1f)))))) ||
               ((iVar17 * 0x59 < iVar7 &&
                 (((((((param_3 == 10 && (*param_2 < 9)) && (param_2[1] < 10)) &&
                      ((iVar3 < 0xde && (iVar1 < 0xd9)))) &&
                     ((param_2[0xb] < 0x1b && ((param_2[10] < 0x2e && (param_2[9] < 0x56)))))) &&
                    (param_2[0x17] + param_2[0x15] < 0x27)) ||
                   (((((iVar17 * 0x59 < iVar7 && (param_3 == 10)) && (*param_2 < 0xc)) &&
                      (((param_2[1] < 0xe && (iVar3 < 0xd3)) &&
                        ((iVar1 < 0xe2 && ((param_2[0xb] < 0x29 && (param_2[10] < 0x2f)))))))) &&
                     ((param_2[9] < 0x52 && (param_2[0x17] + param_2[0x15] < 0x24)))))))))) ||
              (((((((iVar17 * 0x58 < iVar7 && (param_3 == 10)) && (*param_2 < 0xb)) &&
                   (((param_2[1] < 0xb && (iVar3 < 0xca)) &&
                     ((((iVar1 < 0xdd && ((param_2[0xb] < 0x24 && (param_2[10] < 0x27)))) &&
                        (param_2[9] < 0x43)) && (param_2[0x17] + param_2[0x15] < 0x1f)))))) ||
                  ((((((iVar17 * 0x56 < iVar7 && (param_3 == 10)) && (*param_2 < 0xd)) &&
                      ((param_2[1] < 0x10 && (iVar3 < 0xd0)))) && (iVar1 < 0xdb)) &&
                    (((param_2[0xb] < 0x1d && (param_2[10] < 0x3b)) &&
                      ((param_2[9] < 0x58 && (param_2[0x17] + param_2[0x15] < 0x24)))))))) ||
                 (((((((iVar17 * 0x55 < iVar7 && (param_3 == 10)) && (*param_2 < 0xb)) &&
                      ((param_2[1] < 0xf && (iVar3 < 0xca)))) && (iVar1 < 0xdb)) &&
                    (((param_2[0xb] < 0x15 && (param_2[10] < 0x2b)) &&
                      ((param_2[9] < 0x70 && (param_2[0x17] + param_2[0x15] < 0x27)))))) ||
                   ((((iVar17 * 0x5e < iVar7 && (iVar3 < 0xdc)) && (iVar1 < 0xd8)) && (param_2[0xb] < 0x23)
                    ))))) ||
                (((iVar17 * 0x55 < iVar7 && (iVar3 < 0xd7)) &&
                  ((iVar1 < 0xd3 && ((param_2[0xb] < 0x23 && (*param_2 < 0xf))))))))))
            bVar6 = TRUE;
        }
    }
  iVar9 = *param_2;
  iVar7 = iVar9;
  if (((iVar9 < 0x19) && (param_2[0x15] < 0x46)) && (iVar11 = param_2[4], iVar11 < 0xf2))
    {
      iVar7 = iVar8 * 100;
      iVar16 = iVar17 * 0x5f;
      if (((((iVar16 < iVar7) && (iVar3 < 0xd2)) && ((iVar1 < 0xd2 && (iVar2 < 0x51)))) ||
           ((iVar17 * 0x5a < iVar7 &&
             (((iVar3 < 0xcd && (iVar1 < 0xcd)) ||
               ((iVar17 * 0x5a < iVar7 &&
                 ((((*param_2 < 0xc && (iVar3 < 0xd2)) && (iVar1 < 0xd2)) && (param_2[0xb] < 0x1a)))))))))
          ) || ((((iVar16 < iVar7 && (iVar3 < 0xdc)) && (iVar1 < 0xd2)) && (iVar2 < 0x10))))
        bVar6 = TRUE;
      if ((param_3 == 0x18) || (param_3 == 10))
        {
          if (((iVar7 <= iVar17 * 0x62) ||
               (((((0xe5 < iVar3 || (0xd7 < iVar1)) || (0x19 < param_2[0xb])) ||
                  ((0x46 < iVar2 || (0xee < iVar11)))) &&
                 ((((iVar7 <= iVar17 * 0x62 || ((0xea < iVar3 || (0xd7 < iVar1)))) ||
                    (0x1e < param_2[0xb])) || ((0x1f < iVar2 || (0xee < iVar11)))))))) &&
              (((iVar10 = iVar17 * 0x61, iVar7 <= iVar10 ||
                 (((((0xe0 < iVar3 || (0xd2 < iVar1)) || (0x19 < param_2[0xb])) ||
                    ((0x46 < iVar2 || (0xea < iVar11)))) &&
                   ((iVar7 <= iVar10 ||
                     ((((0xe7 < iVar3 || (0x26 < param_2[0xb])) || ((0x20 < iVar2 || (0xed < iVar11)))) &&
                       (((((iVar7 <= iVar10 || (0xe3 < iVar3)) || (0xd2 < iVar1)) ||
                          ((0x1c < param_2[0xb] || (0x28 < iVar2)))) || (0xea < iVar11)))))))))) &&
                (((((iVar7 <= iVar17 * 0x60 || (0xe0 < iVar3)) ||
                    ((0xd2 < iVar1 || (((0x1a < param_2[0xb] || (0x44 < iVar2)) || (0xea < iVar11)))))) &&
                   ((((iVar7 <= iVar16 || (0xdc < iVar3)) || (0xcd < iVar1)) ||
                     (((0x1f < param_2[0xb] || (0x20 < iVar2)) || (0xea < iVar11)))))) &&
                  ((((iVar7 <= iVar17 * 0x5e || (0xdc < iVar3)) || (0xc3 < iVar1)) ||
                    (((0x1f < param_2[0xb] || (0x34 < iVar2)) || (0xea < iVar11))))))))))
            {
              if (((iVar17 * 0x5b < iVar7) && (iVar3 < 0xd2)) &&
                  ((iVar1 < 199 && ((param_2[0xb] < 0x16 && (iVar2 < 0x33))))))
                {
                  iVar16 = *param_2;
                  if ((iVar11 < 0xe6) && (iVar16 < 0x10))
                    goto LAB_18002dcda;
                }
              else
                {
                  iVar16 = *param_2;
                }
              if ((iVar7 <= iVar17 * 0x57) ||
                  ((((0xd7 < iVar3 || (0xb9 < iVar1)) || (0x15 < param_2[0xb])) ||
                    ((0x2a < iVar2 || (0xf < iVar16))))))
                {
                  if (iVar17 * 0x55 < iVar7)
                    {
                      if (((iVar3 < 200) && (iVar1 < 0xc3)) && (iVar2 < 0x47))
                        goto LAB_18002dcda;
                      if (iVar7 <= iVar17 * 0x55)
                        goto LAB_18002dccd;
                      iVar7 = *param_2;
                      if (((iVar3 < 0xd2) && (iVar1 < 0xcd)) && (iVar7 < 0xe))
                        goto LAB_18002dcda;
                    }
                  else
                    {
LAB_18002dccd:
                      iVar7 = *param_2;
                    }
                  if (*param_6 < 7)
                    goto LAB_18002dcfb;
                }
            }
LAB_18002dcda:
          bVar6 = TRUE;
          iVar7 = *param_2;
        }
      else
        {
          iVar7 = *param_2;
        }
    }
LAB_18002dcfb:
  if ((param_3 == 0x18) || (param_3 == 10))
    {
      iVar16 = iVar8 * 100;
      iVar11 = iVar17 * 99;
      if ((((iVar11 < iVar16) &&
            ((((iVar3 < 0xd2 && (param_2[0xb] < 0x25)) && ((iVar2 < 0x42 && (param_2[4] < 0xe7)))) ||
              ((iVar11 < iVar16 &&
                (((((iVar3 < 0xd2 && (iVar1 == 0)) && (param_2[0xb] < 0x31)) &&
                   ((iVar2 < 0x47 && (param_2[4] < 0xe7)))) ||
                  ((((iVar11 < iVar16 && ((iVar3 < 0xd9 && (iVar1 < 0xb3)))) && (param_2[0xb] < 0x13)) &&
                    ((iVar2 < 0x51 && (param_2[4] < 0xf6)))))))))))) ||
           (((iVar17 * 0x62 < iVar16 &&
              (((((iVar3 < 0xdc && (iVar1 < 0xd0)) && (param_2[0xb] < 0x1f)) &&
                 ((iVar2 < 0x51 && (param_2[4] < 0xe9)))) ||
                (((iVar17 * 0x62 < iVar16 && ((iVar3 < 0xe1 && (iVar1 < 0xd8)))) &&
                  ((param_2[0xb] < 0x24 && ((iVar2 < 0x44 && (param_2[4] < 0xeb)))))))))) ||
             ((iVar11 = iVar17 * 0x61, iVar11 < iVar16 &&
               (((((iVar3 < 0xdc && (iVar1 < 0xce)) && (param_2[0xb] < 0x15)) &&
                  ((iVar2 < 0x47 && (param_2[4] < 0xe9)))) ||
                 ((iVar11 < iVar16 &&
                   ((((iVar3 < 0xd7 && (param_2[0xb] < 0x29)) && ((iVar2 < 0x47 && (param_2[4] < 0xe9)))) ||
                     ((iVar11 < iVar16 &&
                       (((((iVar3 < 0xe0 && (iVar1 < 0xce)) && (param_2[0xb] < 0x15)) &&
                          (((iVar2 < 0x51 && (param_2[4] < 0xe9)) && (iVar12 == 1)))) ||
                         (((iVar11 < iVar16 && (iVar3 < 0xd2)) &&
                           ((iVar1 == 0 &&
                             ((((param_2[0xb] < 0x30 && (iVar2 < 0x47)) && (param_2[4] < 0xe9)) &&
                               (iVar12 == 1)))))))))))))))))))))) ||
          ((((((iVar17 * 0x5f < iVar16 && (iVar3 < 0xdd)) &&
               ((iVar1 < 0xcd && (((param_2[0xb] < 0x17 && (iVar2 < 0x4f)) && (param_2[4] < 0xe9)))))) ||
              (((((iVar17 * 0x5f <= iVar16 && (iVar3 < 0xdb)) && (iVar1 < 0xb7)) &&
                 ((param_2[0xb] < 0xe && (iVar2 < 0x37)))) && (param_2[4] < 0xf6)))) ||
             (((iVar17 * 0x5e < iVar16 && (iVar3 < 0xda)) &&
               ((iVar1 < 0xcf && (((param_2[0xb] < 0x15 && (iVar2 < 0x51)) && (param_2[4] < 0xfb)))))))) ||
            (((((iVar17 * 0x5d < iVar16 && (iVar14 < 0x178)) && (param_2[0xb] < 0x15)) &&
               ((iVar2 < 0x56 && (param_2[4] < 0xe7)))) ||
              ((iVar17 * 0x5b < iVar16 &&
                (((((iVar3 < 0xd5 && (iVar1 < 0xcd)) && (param_2[0xb] < 0x17)) &&
                   ((iVar2 < 0x50 && (param_2[4] < 0xe5)))) ||
                  ((((iVar17 * 0x5b < iVar16 && ((iVar3 < 0xd8 && (iVar1 < 0xbf)))) &&
                     (param_2[0xb] < 0x12)) && ((iVar2 < 0x33 && (param_2[4] < 0xf9))))))))))))))
        bVar6 = TRUE;
    }
  if ((2 < iVar18) &&
      (((((iVar3 < 0xd7 && (iVar1 < 0xd7)) && (iVar7 < 0xb)) && (iVar17 * 0x50 < iVar8 * 100)) ||
        (((iVar3 < 0xdc && (iVar1 < 0xdc)) && (iVar17 * 0x5a < iVar8 * 100))))))
    bVar6 = TRUE;
  if ((1 < iVar18) &&
      ((((((iVar3 < 0xd7 && (iVar1 < 0xd7)) && ((iVar7 < 0x10 && (iVar17 * 0x5f < iVar8 * 100)))) ||
          (((((iVar11 = iVar8 * 100, iVar17 * 0x5d < iVar11 && (param_3 == 10)) && (iVar7 < 0xd)) &&
             ((param_2[1] < 0xe && (iVar3 < 0xc3)))) &&
            ((iVar1 < 0xdd &&
              (((param_2[0xd] == 1 && (param_2[0xb] < 0x30)) &&
                ((param_2[10] < 0x27 && ((param_2[9] < 0x52 && (param_2[0x17] + param_2[0x15] < 0x2e)))))
               ))))))) ||
         (((iVar17 * 0x53 < iVar11 &&
            (((((param_3 == 10 && (iVar7 < 8)) && (param_2[1] < 0xc)) &&
               ((iVar3 < 0xd8 && (iVar1 < 0xce)))) && (param_2[0xd] == 1)))) &&
           (((param_2[0xb] < 0x15 && (param_2[10] < 0x2b)) &&
             ((param_2[9] < 0x55 && ((param_2[0x17] + param_2[0x15] < 99 && (param_2[0x15] < 0x1a)))))))
          ))) || ((iVar17 * 0x5e < iVar11 &&
                   ((((param_3 == 10 && (iVar7 < 0xb)) && (param_2[1] < 0xc)) &&
                     (((iVar14 < 0x1ad && (param_2[0xb] < 0x29)) &&
                       ((param_2[10] < 0x2d &&
                         ((param_2[9] < 0x48 && (param_2[0x17] + param_2[0x15] < 0x21))))))))))))))
    bVar6 = TRUE;
  iVar7 = iVar17 * 0x5a;
  iVar8 = iVar8 * 100;
  if (((((iVar7 < iVar8) && (iVar3 < 0xcd)) && (iVar1 < 0xd3)) &&
       (((param_2[0xb] < 0x2d && (iVar2 < 0x33)) && (param_2[4] < 0xf0)))) ||
      (((iVar17 * 0x5f < iVar8 && (iVar3 < 0xd8)) &&
        ((iVar1 < 0xd8 && ((param_2[0xb] < 0x23 && (param_2[4] < 0xf0))))))))
    bVar6 = TRUE;
  iVar11 = param_2[0x17];
  iVar16 = param_2[9];
  iVar10 = param_2[0xb];
  iVar4 = param_2[10];
  iVar5 = param_2[0xd];
  iVar13 = param_2[0xe];
  if (param_3 != 10)
    {
      if (((((guint32) param_3 - 7U) & 0xffffffef) == 0) &&
          (((((((iVar17 * 0x50 < iVar8 && (iVar2 < 0x51)) && (4 < iVar12)) &&
               ((3 < iVar18 && (iVar1 < 0xc3)))) ||
              (((((iVar14 = iVar9, iVar17 * 0x55 < iVar8 && ((iVar2 < 0x47 && (4 < iVar12)))) &&
                  (iVar3 < 0xdc)) &&
                 (((iVar1 < 200 && (iVar10 < 0xf)) && (iVar14 = *param_2, iVar14 < 10)))) ||
                ((((iVar11 = iVar17 * 0x5f, iVar11 < iVar8 && (iVar2 < 0x51)) && (3 < iVar12)) ||
                  (((iVar7 < iVar8 && (iVar3 < 0xf0)) &&
                    ((iVar1 < 0xdc && ((1 < iVar18 && (3 < iVar12)))))))))))) ||
             ((iVar11 < iVar8 &&
               (((((iVar3 < 0xf0 && (iVar1 < 0xdc)) && (iVar2 < 0x51)) && (2 < iVar12)) ||
                 ((iVar11 < iVar8 &&
                   ((((iVar3 < 0xe6 && (iVar1 < 0xdc)) && ((iVar2 < 0x4c && (1 < iVar12)))) ||
                     ((((iVar11 < iVar8 && (iVar3 < 0xe6)) && (iVar1 < 0xbe)) && (1 < iVar12)))))))))))) ||
            (((iVar17 * 0x61 < iVar8 &&
               ((((((iVar3 < 0xd7 && (iVar1 < 0xc3)) && (iVar10 < 0xf)) &&
                   ((iVar14 < 0xe && (iVar2 < 0x51)))) && (0 < iVar12)) ||
                 (((iVar17 * 0x61 < iVar8 && (iVar3 < 0xdc)) &&
                   ((iVar1 < 0xd2 && ((iVar2 < 0x47 && (0 < iVar12)))))))))) ||
              (((iVar7 < iVar8 &&
                 (((((iVar3 < 0xeb && (iVar1 < 0xb9)) && (iVar2 < 0x47)) && ((0 < iVar12 && (iVar14 < 8)))
                   ) || (((iVar7 < iVar8 && ((iVar3 < 0xf0 && (iVar1 < 0xdc)))) && (2 < iVar18)))))) ||
                ((((iVar11 < iVar8 && (iVar3 < 0xe6)) && (iVar1 < 0xdc)) && (1 < iVar18))))))))))
        bVar6 = TRUE;
      goto LAB_18002fa16;
    }
  if (iVar2 == 0)
    {
      iVar12 = iVar17 * 99;
      iVar18 = iVar3 + iVar1;
      if (((((((iVar12 < iVar8) &&
               (((((iVar18 < 0x1c6 && (iVar11 < 0x10)) && (iVar16 < 0x49)) &&
                  ((iVar10 < 0x2e || ((iVar10 < 0x39 && (iVar5 == 1)))))) ||
                 ((iVar12 < iVar8 &&
                   (((((iVar18 < 0x19a && (iVar11 < 0x25)) && (iVar10 < 0x2b)) &&
                      ((iVar4 < 0x35 && (iVar16 < 0x49)))) ||
                     (((iVar12 < iVar8 && ((iVar5 == 1 && (iVar18 < 0x1a2)))) &&
                       ((iVar11 < 0x3f && ((iVar10 < 0x2b && (iVar16 < 0x44)))))))))))))) ||
              ((iVar17 * 0x61 < iVar8 &&
                (((((iVar18 < 0x1b1 && (iVar11 < 0x13)) && (iVar10 < 0x2f)) &&
                   ((iVar16 < 0x40 && (iVar5 == 1)))) ||
                  ((((iVar17 * 0x61 < iVar8 && ((iVar18 < 0x19a && (iVar11 < 0x38)))) && (iVar10 < 0x2f)) &&
                    ((iVar16 < 0x43 && (iVar5 == 1)))))))))) ||
             ((iVar17 * 0x5f < iVar8 &&
               (((((iVar18 < 0x1b1 && (iVar11 < 0x27)) && (iVar10 < 0x2f)) && (iVar16 < 0x55)) ||
                 ((((iVar17 * 0x5f < iVar8 && (iVar5 == 1)) &&
                    ((iVar18 < 0x1b0 && ((iVar11 < 0x1a && (iVar10 < 0x35)))))) && (iVar16 < 0x4c)))))))) ||
            (((((iVar17 * 0x5e < iVar8 && (iVar5 == 1)) && (iVar18 < 0x1a6)) &&
               ((iVar11 < 0x21 && (iVar10 < 0x35)))) && (iVar16 < 0x44)))) ||
           (((iVar12 = iVar17 * 0x5d, iVar12 < iVar8 &&
              ((((iVar5 == 1 && (iVar18 < 0x1c4)) &&
                 ((iVar11 < 0x13 && ((iVar10 < 0x29 && (iVar16 < 0x46)))))) ||
                ((iVar12 < iVar8 &&
                  (((((iVar18 < 0x1be && (iVar11 < 0x18)) && (iVar10 < 0x37)) && (iVar16 < 0x52)) ||
                    ((iVar12 < iVar8 &&
                      ((((iVar18 < 0x19f && (iVar11 < 0x29)) && ((iVar10 < 0x29 && (iVar16 < 0x55)))) ||
                        ((iVar12 < iVar8 &&
                          ((((iVar18 < 0x1b3 && (iVar11 < 0x1f)) && ((iVar10 < 0x29 && (iVar16 < 0x41)))) ||
                            ((iVar12 < iVar8 &&
                              (((((iVar18 < 0x1a4 && (iVar11 < 0x15)) && (iVar10 < 0x1a)) && (iVar16 < 0x69)) ||
                                ((((iVar12 < iVar8 && (iVar5 == 1)) &&
                                   ((iVar18 < 0x1c2 && ((iVar11 < 0x1f && (iVar10 < 0x38)))))) && (iVar16 < 0x70))
                                )))))))))))))))))))) ||
             ((iVar12 = iVar17 * 0x5c, iVar12 < iVar8 &&
               (((((iVar5 == 1 && (iVar18 < 0x1ae)) && (iVar11 < 0x1f)) &&
                  ((iVar10 < 0x2f && (iVar16 < 0x44)))) ||
                 ((iVar12 < iVar8 &&
                   ((((iVar18 < 0x1ae && (iVar11 < 0x17)) && ((iVar10 < 0x2b && (iVar16 < 0x50)))) ||
                     ((iVar12 < iVar8 &&
                       (((((iVar5 == 1 && (iVar18 < 0x1bd)) && (iVar11 < 0x10)) &&
                          ((iVar10 < 0x33 && (iVar16 < 0x3c)))) ||
                         ((((iVar12 < iVar8 && ((iVar18 < 0x19f && (iVar11 < 0x1f)))) && (iVar10 < 0x24)) &&
                           ((iVar4 < 0x2e && (iVar16 < 0x4b)))))))))))))))))))) ||
          (((iVar17 * 0x5b < iVar8 &&
             (((((iVar5 == 1 && (iVar18 < 0x1ac)) && (iVar11 < 0x2e)) &&
                ((iVar10 < 0x2d && (iVar16 < 0x55)))) ||
               (((iVar17 * 0x5b < iVar8 && ((iVar18 < 0x1ac && (iVar11 < 0x2e)))) &&
                 ((iVar10 < 0x2d && (iVar16 < 0x55)))))))) ||
            ((((iVar7 < iVar8 &&
                (((((iVar5 == 1 && (iVar18 < 0x1b6)) && (iVar11 < 0x15)) &&
                   ((iVar10 < 0x35 && (iVar16 < 0x52)))) ||
                  ((iVar7 < iVar8 &&
                    ((((iVar5 == 1 && (iVar18 < 0x19a)) &&
                       ((iVar11 < 0x33 && ((iVar10 < 0x29 && (iVar16 < 0x4e)))))) ||
                      ((iVar7 < iVar8 &&
                        ((((iVar13 == 1 && (iVar18 < 0x1b4)) && (iVar11 < 0x2e)) &&
                          ((iVar10 < 0x2d && (iVar16 < 0x4c)))))))))))))) ||
               ((iVar17 * 0x59 < iVar8 &&
                 ((((iVar18 < 0x1a9 && (iVar11 < 0x15)) && ((iVar10 < 0x2a && (iVar16 < 0x71)))) ||
                   ((((iVar17 * 0x59 < iVar8 && (iVar13 == 1)) && (iVar18 < 0x1a9)) &&
                     (((iVar11 < 0x21 && (iVar10 < 0x29)) && (iVar16 < 0x44)))))))))) ||
              (((((iVar17 * 0x58 < iVar8 && (iVar5 == 1)) &&
                  ((iVar18 < 0x1ae && (((iVar11 < 0x17 && (iVar10 < 0x34)) && (iVar16 < 0x70)))))) ||
                 (((iVar17 * 0x57 < iVar8 && (iVar18 < 0x19f)) &&
                   ((iVar11 < 0x15 && ((iVar10 < 0x2a && (iVar16 < 0x4b)))))))) ||
                ((iVar17 * 0x55 < iVar8 &&
                  ((((iVar13 == 1 && (iVar18 < 0x1a1)) && (iVar11 < 0x11)) &&
                    ((iVar10 < 0x15 && (iVar16 < 0x6c))))))))))))))
        bVar6 = TRUE;
    }
  iVar18 = iVar17 * 0x61;
  if ((((iVar18 < iVar8) && (iVar3 + iVar1 < 0x19a)) &&
       ((iVar10 < 0x33 && (((iVar4 < 0x42 && (iVar16 < 100)) && (iVar11 < 0x29)))))) &&
      (iVar2 < 0x1f))
    {
LAB_18002f077:
      iVar12 = param_2[0xe];
LAB_18002f07b:
      bVar6 = TRUE;
    }
  else
    {
      iVar13 = iVar17 * 0x5e;
      iVar12 = iVar3 + iVar1;
      if (((iVar13 < iVar8) &&
           ((((iVar12 < 0x1b4 && (iVar10 < 0x18)) &&
              (((iVar4 < 0x2c && ((iVar16 < 0x78 && (iVar11 < 0x18)))) && (iVar2 < 0xe)))) ||
             ((iVar13 < iVar8 &&
               (((((iVar12 < 0x196 && (iVar10 < 0x18)) && (iVar4 < 0x29)) &&
                  ((iVar16 < 100 && (iVar2 < 0x1a)))) ||
                 (((iVar13 < iVar8 && ((iVar12 < 0x1a9 && (iVar10 < 0x29)))) &&
                   ((iVar4 < 0x2b && (((iVar16 < 0x66 && (iVar11 < 0x2e)) && (iVar2 < 0x15)))))))))))))) ||
          ((iVar13 = iVar17 * 0x5d, iVar13 < iVar8 &&
            (((((iVar12 < 0x1a9 && (iVar10 < 0x21)) &&
                ((iVar4 < 0x38 && ((iVar16 < 0x60 && (iVar11 < 0x1a)))))) && (iVar2 < 0x1f)) ||
              ((iVar13 < iVar8 &&
                ((((((iVar12 < 0x1a4 && (iVar10 < 0x15)) && (iVar4 < 0x33)) &&
                    ((iVar16 < 0x80 && (iVar11 < 0x29)))) && (iVar2 < 0x35)) ||
                  ((iVar13 < iVar8 &&
                    ((((iVar12 < 0x19f && (iVar10 < 0x13)) &&
                       ((iVar4 < 0x2b && (((iVar16 < 0x84 && (iVar11 < 0x1f)) && (iVar2 < 0x15)))))) ||
                      ((iVar13 < iVar8 &&
                        (((((iVar12 < 0x1b3 && (iVar10 < 0x17)) &&
                            ((iVar4 < 0x2b && ((iVar16 < 100 && (iVar11 < 0x2e)))))) && (iVar2 < 0x1a)) ||
                          ((iVar13 < iVar8 &&
                            ((((((iVar5 == 1 && (iVar12 < 0x1ba)) && (iVar10 < 0x1a)) &&
                                ((iVar4 < 0x29 && (iVar16 < 0x80)))) && ((iVar11 < 0x18 && (iVar2 < 0xe)))) ||
                              ((iVar13 < iVar8 &&
                                ((((iVar5 == 1 && (iVar12 < 0x1a9)) &&
                                   ((iVar10 < 0x1f &&
                                     ((((iVar4 < 0x3d && (iVar16 < 0x69)) && (iVar11 < 0x15)) && (iVar2 < 0xd)))))) ||
                                  (((iVar13 < iVar8 && (iVar5 == 1)) &&
                                    ((iVar12 < 0x19f &&
                                      (((iVar10 < 0x1f && (iVar4 < 0x2b)) &&
                                        ((iVar16 < 0x6d && ((iVar11 < 0x2e && (iVar2 < 0x1f))))))))))))))))))))))))
                    ))))))))))))
        goto LAB_18002f077;
      iVar12 = param_2[0xe];
      iVar13 = iVar17 * 0x5c;
      if ((((iVar13 < iVar8) &&
            (((((iVar12 == 1 && (iVar14 < 0x1ae)) && (iVar10 < 0x18)) &&
               (((iVar4 < 0x25 && (iVar16 < 0x7a)) && ((iVar11 < 0x13 && (iVar2 < 0x27)))))) ||
              ((iVar13 < iVar8 &&
                (((((iVar5 == 1 && (iVar14 < 0x1a9)) &&
                    ((iVar10 < 0x1a && (((iVar4 < 0x29 && (iVar16 < 0x87)) && (iVar11 < 0x18)))))) &&
                   (iVar2 < 0x29)) ||
                  ((iVar13 < iVar8 &&
                    ((((iVar14 < 0x1c5 && (iVar10 < 0x18)) &&
                       (((iVar4 < 0x27 && ((iVar16 < 0x80 && (iVar11 < 0x15)))) && (iVar2 < 0x15)))) ||
                      ((((iVar13 < iVar8 && (iVar3 + iVar1 < 0x18b)) && (iVar10 < 0x15)) &&
                        ((iVar4 < 0x29 && (iVar16 < 0x7d)))))))))))))))) ||
           ((iVar13 = iVar17 * 0x5b, iVar13 < iVar8 &&
             (((((iVar12 == 1 && (iVar14 < 400)) &&
                 ((iVar10 < 0x12 && (((iVar4 < 0x29 && (iVar16 < 0x7e)) && (iVar11 < 0x1a)))))) &&
                (iVar2 < 0x24)) ||
               ((iVar13 < iVar8 &&
                 (((((iVar5 == 1 && (iVar14 < 400)) &&
                     ((iVar10 < 0x15 && ((iVar4 < 0x27 && (iVar16 < 0x84)))))) &&
                    ((iVar11 < 0x16 && (iVar2 < 0x1d)))) ||
                   ((iVar13 < iVar8 &&
                     (((((iVar5 == 1 && (iVar14 < 0x182)) && (iVar10 < 0x17)) &&
                        (((iVar4 < 0x39 && (iVar16 < 0x84)) && ((iVar11 < 0x33 && (iVar2 < 0x33)))))) ||
                       ((iVar13 < iVar8 &&
                         ((((iVar5 == 1 && (iVar14 < 0x1a4)) &&
                            ((iVar10 < 0x18 &&
                              ((((iVar4 < 0x2b && (iVar16 < 0x7d)) && (iVar11 < 0x18)) && (iVar2 < 0xd)))))) ||
                           (((((iVar13 < iVar8 && (iVar3 + iVar1 < 0x19a)) && (iVar11 < 0x15)) &&
                              ((iVar10 < 0x1d && (iVar4 < 0x2e)))) && (iVar16 < 0x80)))))))))))))))))))) ||
          ((iVar7 < iVar8 &&
            ((((((iVar3 + iVar1 < 0x1a9 && (iVar10 < 0x15)) && (iVar4 < 0x2b)) &&
                ((iVar16 < 0x6c && (iVar11 < 0x15)))) && (iVar2 < 0xe)) ||
              ((iVar7 < iVar8 &&
                ((((iVar5 == 1 && (iVar3 + iVar1 < 0x177)) &&
                   ((iVar10 < 0x1a && (((iVar4 < 0x27 && (iVar16 < 0x87)) && (0x8c < iVar11 + iVar2)))))) ||
                  (((iVar7 < iVar8 && (iVar3 + iVar1 < 0x19f)) &&
                    ((iVar11 < 0x15 && (((iVar10 < 0x1d && (iVar4 < 0x2e)) && (iVar16 < 0x5c)))))))))))))
           )))
        goto LAB_18002f07b;
      iVar15 = iVar3 + iVar1;
      iVar13 = iVar17 * 0x59;
      if ((iVar13 < iVar8) &&
          (((((((iVar5 == 1 && (iVar15 < 0x1b9)) && (iVar10 < 0x18)) &&
               ((iVar4 < 0x29 && (iVar16 < 0x7e)))) && (iVar11 < 0x13)) && (iVar2 < 0xd)) ||
            ((iVar13 < iVar8 &&
              ((((iVar15 < 0x192 && (iVar10 < 0x18)) &&
                 ((iVar4 < 0x2e && (((iVar16 < 0x7e && (iVar11 < 0x1a)) && (iVar2 < 0x13)))))) ||
                ((iVar13 < iVar8 &&
                  ((((iVar15 < 0x1a1 && (iVar10 < 0x1b)) &&
                     (((iVar4 < 0x2e && ((iVar16 < 0x7e && (iVar11 < 0x21)))) && (iVar2 < 0x1c)))) ||
                    ((iVar13 < iVar8 &&
                      (((((iVar15 < 0x1a7 && (iVar10 < 0x11)) && (iVar4 < 0x33)) &&
                         (((iVar16 < 0x70 && (iVar11 < 0x24)) && (iVar2 < 0x2d)))) ||
                        (((iVar13 < iVar8 && (iVar3 + iVar1 < 0x198)) &&
                          ((iVar10 < 0xd &&
                            ((((iVar4 < 0x33 && (iVar16 < 0x7e)) && (iVar11 < 0x3d)) && (iVar2 < 0x3d)))))))))))
                   )))))))))))
        goto LAB_18002f07b;
      iVar13 = iVar3 + iVar1;
      iVar15 = iVar17 * 0x58;
      if (((iVar15 < iVar8) &&
           (((((iVar13 < 0x195 && (iVar10 < 0x1b)) &&
               ((iVar4 < 0x3d && ((iVar16 < 0x76 && (iVar11 < 0x37)))))) && (iVar2 < 0x3f)) ||
             ((iVar15 < iVar8 &&
               (((((iVar12 == 1 && (iVar13 < 0x1ae)) && (iVar10 < 0x18)) &&
                  (((iVar4 < 0x25 && (iVar16 < 0x69)) && ((iVar11 < 0x13 && (iVar2 < 0xb)))))) ||
                 ((iVar15 < iVar8 &&
                   ((((iVar12 == 1 && (iVar13 < 0x1a4)) &&
                      (((iVar10 < 0x18 && (((iVar4 < 0x25 && (iVar16 < 0x73)) && (iVar11 < 0x13)))) &&
                        (iVar2 < 0xb)))) ||
                     ((iVar15 < iVar8 &&
                       ((((((iVar5 == 1 && (iVar13 < 0x195)) &&
                            ((iVar10 < 0x18 && ((iVar4 < 0x29 && (iVar16 < 0x77)))))) && (iVar11 < 0x25)) &&
                          (iVar2 < 0x10)) ||
                         ((iVar15 < iVar8 &&
                           ((((((iVar5 == 1 && (iVar13 < 0x18b)) && (iVar10 < 0x15)) &&
                               ((iVar4 < 0x33 && (iVar16 < 0x81)))) && (0x6d < iVar11 + iVar2)) ||
                             (((iVar15 < iVar8 && (iVar3 + iVar1 < 0x195)) &&
                               ((iVar11 + iVar2 < 0x3d &&
                                 (((iVar10 < 0x18 && (iVar4 < 0x2b)) && (iVar16 < 0x78)))))))))))))))))))))))))) ||
          (((((iVar15 < iVar8 && (iVar3 + iVar1 < 0x19f)) && (iVar11 + iVar2 < 0x45)) &&
             ((iVar10 < 0x17 && (iVar4 < 0x33)))) && (iVar16 < 0x7d))))
        goto LAB_18002f07b;
      iVar13 = iVar17 * 0x57;
      if (((((iVar13 < iVar8) &&
             ((((((iVar5 == 1 && (iVar3 + iVar1 < 0x1a4)) && (iVar10 < 0x15)) &&
                 ((iVar4 < 0x21 && (iVar16 < 0x73)))) && ((iVar11 < 0x13 && (iVar2 < 0x15)))) ||
               ((((iVar13 < iVar8 && (iVar5 == 1)) && (iVar3 + iVar1 < 0x195)) &&
                 (((iVar10 < 0x14 && (iVar16 < 0x73)) && (0x68 < iVar11 + iVar2)))))))) ||
            (((((iVar17 * 0x56 < iVar8 && (iVar12 == 1)) &&
                ((((iVar14 < 0x19a && ((iVar10 < 0x13 && (iVar4 < 0x27)))) && (iVar16 < 0x73)) &&
                  ((iVar11 < 0x13 && (iVar2 < 0x17)))))) ||
               (((iVar15 = iVar3 + iVar1, iVar13 < iVar8 &&
                  (((((iVar15 < 0x192 && (iVar10 < 0x13)) && (iVar4 < 0x2e)) &&
                     ((iVar16 < 0x7e && (iVar11 < 0x40)))) && (iVar2 < 0x3d)))) ||
                 ((((iVar17 * 0x56 < iVar8 && (iVar15 < 0x18b)) && (iVar11 < 0x33)) &&
                   (((iVar10 < 0x1a && (iVar4 < 0x33)) && (iVar16 < 0x7a)))))))) ||
              (((((iVar17 * 0x55 < iVar8 && (iVar15 < 0x198)) &&
                  ((iVar10 < 0x10 && ((iVar4 < 0x35 && (iVar16 < 0x6d)))))) &&
                 ((iVar11 < 0x27 && (iVar2 < 0x2c)))) ||
                ((((((iVar17 * 0x53 < iVar8 && (iVar15 < 0x198)) && (iVar11 + iVar2 < 0x3b)) &&
                    ((iVar10 < 0x17 && (iVar4 < 0x34)))) && (iVar16 < 0x7a)) ||
                  (((iVar13 = iVar3 + iVar1, iVar17 * 0x52 < iVar8 && (iVar5 == 1)) &&
                    ((iVar13 < 0x18c &&
                      ((((iVar10 < 0x18 && (iVar4 < 0x38)) && (iVar16 < 0x7d)) &&
                        ((iVar11 < 0x2f && (iVar2 < 0x38)))))))))))))))) ||
           ((iVar15 = iVar17 * 0x51, iVar15 < iVar8 &&
             ((((iVar13 < 0x18c && (iVar10 < 0x24)) &&
                ((iVar4 < 0x30 && (((iVar16 < 0x74 && (iVar11 < 0x40)) && (iVar2 < 0x21)))))) ||
               ((iVar15 < iVar8 &&
                 (((((iVar5 == 1 && (iVar13 < 0x195)) &&
                     ((iVar10 < 0x18 && ((iVar4 < 0x2e && (iVar16 < 0x73)))))) &&
                    ((iVar11 < 0x1a && (iVar2 < 0x16)))) ||
                   ((iVar15 < iVar8 &&
                     ((((((iVar5 == 1 && (iVar13 < 0x188)) && (iVar10 < 0x1d)) &&
                         ((iVar4 < 0x2e && (iVar16 < 0x78)))) && ((iVar11 < 0x1a && (iVar2 < 0x2e)))) ||
                       ((iVar15 < iVar8 &&
                         ((((iVar13 < 0x18c && (iVar11 + iVar2 < 0x4f)) &&
                            ((iVar10 < 0x1a && ((iVar4 < 0x33 && (iVar16 < 0x78)))))) ||
                           ((iVar15 < iVar8 &&
                             (((((iVar3 + iVar1 < 0x198 && (iVar11 + iVar2 < 0x3b)) && (iVar10 < 0x1a)) &&
                                ((iVar4 < 0x1f && (iVar16 < 0x6c)))) ||
                               ((((iVar15 < iVar8 && ((iVar3 + iVar1 < 0x183 && (iVar11 + iVar2 < 0x6a)))) &&
                                  (iVar10 < 0x15)) && ((iVar4 < 0x35 && (iVar16 < 0x73)))))))))))))))))))))))))) ||
          ((((iVar17 * 0x4e < iVar8 &&
              (((iVar3 + iVar1 < 0x185 && (iVar11 + iVar2 < 0x4c)) && (iVar10 < 0x1a)))) &&
             ((iVar4 < 0x29 && (iVar16 < 0x69)))) ||
            ((iVar17 * 0x4d < iVar8 &&
              (((iVar3 + iVar1 < 0x18b && (iVar11 + iVar2 < 0x51)) &&
                ((iVar10 < 0x1a && ((iVar4 < 0x38 && (iVar16 < 0x70))))))))))))
        goto LAB_18002f07b;
    }
  if (((((((iVar17 * 0x5e < iVar8) && (iVar3 + iVar1 < 0x186)) && (*param_2 < 0x10)) &&
         (((param_2[1] < 0x14 && (iVar10 < 0x13)) &&
           ((iVar4 < 0x29 && ((iVar16 < 0x88 && (iVar5 == 1)))))))) ||
        ((iVar17 * 0x5d < iVar8 &&
          ((((((iVar3 + iVar1 < 400 && (*param_2 < 0x14)) && (param_2[1] < 0x14)) &&
              ((iVar10 < 0x14 && (iVar4 < 0x31)))) && (iVar16 < 0x88)) && (0x8b < iVar11 + iVar2)))))) ||
       ((iVar17 * 0x5c < iVar8 &&
         (((((iVar14 < 0x187 && (*param_2 < 0x13)) &&
             ((param_2[1] < 0x13 && (((iVar10 < 0x13 && (iVar4 < 0x29)) && (iVar16 < 0x84)))))) &&
            (param_2[4] == 0x80)) ||
           (((iVar17 * 0x5c < iVar8 && (iVar3 + iVar1 < 0x18c)) &&
             (((iVar10 < 0x13 && ((iVar4 < 0x38 && (iVar16 < 0x84)))) && (0x9f < iVar11 + iVar2))))))
         )))) ||
      (((((iVar17 * 0x56 < iVar8 && (iVar3 + iVar1 < 0x183)) && (iVar10 < 0x1a)) &&
         (((iVar4 < 0x35 && (iVar16 < 0x78)) && (0x8b < iVar11 + iVar2)))) ||
        ((((iVar17 * 0x54 < iVar8 && (iVar3 + iVar1 < 0x189)) &&
           ((iVar10 < 0x16 && (((iVar4 < 0x36 && (iVar16 < 0x78)) && (0x9f < iVar11 + iVar2)))))) ||
          (((*param_2 < 0xd && (param_2[1] < 0x12)) &&
            ((iVar3 + iVar1 < 0x17c &&
              (((iVar10 < 0x23 && (iVar4 < 0x43)) && ((iVar16 < 0x55 && (iVar2 < 0x25))))))))))))))
    bVar6 = TRUE;
  iVar13 = iVar17 * 0x62;
  if ((((((((iVar13 < iVar8) &&
            (((((iVar3 < 0xda && (iVar1 < 0xc3)) && (iVar2 < 0x1f)) && (iVar10 < 0x13)) ||
              ((iVar13 < iVar8 &&
                ((((iVar14 < 0x1a6 && (iVar2 < 0x16)) && ((iVar10 < 0x1c && (iVar4 < 0x37)))) ||
                  ((((iVar13 < iVar8 && (iVar3 < 0xd4)) && (iVar1 < 200)) &&
                    ((iVar2 < 0x15 && (iVar10 < 0xf)))))))))))) ||
           ((iVar18 < iVar8 &&
             (((((iVar3 < 0xd7 && (iVar1 < 200)) && (iVar2 < 0x15)) && (iVar10 < 0x19)) ||
               (((iVar18 < iVar8 && (iVar3 < 0xdb)) &&
                 (((iVar1 < 0xce && ((iVar2 < 0x12 && (iVar10 < 0x16)))) && (iVar4 < 0x27)))))))))) ||
          (((((iVar17 * 0x5f < iVar8 && (iVar3 < 0xd8)) && (iVar1 < 0xc6)) &&
             ((iVar2 < 0x29 && (iVar10 < 0x18)))) ||
            ((((iVar17 * 0x5e < iVar8 && ((iVar3 < 0xc0 && (iVar1 < 0xd2)))) &&
               ((iVar2 < 0x3d && (iVar10 < 0x12)))) ||
              ((iVar14 = iVar17 * 0x5d, iVar14 < iVar8 &&
                (((((iVar3 < 0xd4 && (iVar1 < 0xce)) && (iVar10 < 0x14)) && (param_2[4] < 0xe7)) ||
                  (((iVar14 < iVar8 && (iVar3 + iVar1 < 0x199)) &&
                    ((iVar10 < 0x12 && ((param_2[4] < 0xe2 && (iVar2 < 0x1f)))))))))))))))) ||
         ((iVar18 = iVar3 + iVar1, iVar14 < iVar8 &&
           (((((iVar18 < 399 && (iVar10 < 0x12)) && (param_2[4] < 0xe2)) && (iVar2 < 0x3d)) ||
             ((iVar14 < iVar8 &&
               ((((iVar3 < 0xe2 && (iVar1 < 0xbf)) &&
                  ((iVar10 < 0xd && ((param_2[4] < 0xfb && (param_2[0x15] < 0x14)))))) ||
                 ((iVar14 < iVar8 &&
                   (((((iVar3 < 0xdd && (iVar1 < 0xba)) && (iVar10 < 0xd)) &&
                      ((param_2[4] < 0xfb && (param_2[0x15] < 0x28)))) ||
                     ((iVar14 < iVar8 &&
                       ((((iVar3 < 0xd2 && (iVar1 < 0xc1)) && ((iVar2 < 0x29 && (iVar10 < 0x10)))) ||
                         ((iVar14 < iVar8 &&
                           (((((iVar3 < 0xd2 && (iVar1 < 0xc3)) && (iVar2 < 0x1d)) && (iVar10 < 0x15)) ||
                             (((iVar14 < iVar8 && (iVar3 < 0xd4)) &&
                               ((iVar1 < 0xcb && ((iVar2 < 0x1a && (iVar10 < 0x15)))))))))))))))))))))))))))) ||
        (((iVar14 = iVar17 * 0x5c, iVar14 < iVar8 &&
           (((((iVar3 < 0xd2 && (iVar1 < 0xcb)) && (iVar2 < 0x29)) && (iVar10 < 0x11)) ||
             ((iVar14 < iVar8 &&
               ((((((iVar3 < 0xd4 && (iVar1 < 0xce)) &&
                    ((iVar10 < 0x11 && ((iVar4 < 0x33 && (iVar16 < 0x85)))))) && (iVar11 < 0x29)) &&
                  (iVar2 < 0x34)) ||
                 ((((iVar14 < iVar8 && (iVar3 < 0xe1)) && (iVar1 < 0xb9)) &&
                   ((iVar2 < 0x33 && (iVar10 < 0x13)))))))))))) ||
          ((iVar14 = iVar17 * 0x5b, iVar14 < iVar8 &&
            ((((iVar3 < 0xd4 && (iVar1 < 0xc0)) && ((iVar2 < 0x23 && (iVar10 < 0x15)))) ||
              ((iVar14 < iVar8 &&
                (((((iVar3 < 0xd3 && (iVar1 < 0xc3)) && (iVar2 < 0x33)) && (iVar10 < 0xd)) ||
                  (((iVar14 < iVar8 && (iVar3 < 0xc4)) &&
                    ((iVar1 < 0xcf && ((iVar2 < 0x39 && (iVar10 < 0x17)))))))))))))))))) ||
       ((iVar7 < iVar8 &&
         (((((iVar3 < 0xc1 && (iVar1 < 0xd9)) && (iVar2 < 0x14)) &&
            (((iVar10 < 0x1a && (iVar4 < 0x29)) && (iVar5 == 1)))) ||
           ((iVar7 < iVar8 &&
             ((((iVar3 < 200 && (iVar1 < 0xd9)) &&
                ((iVar2 < 0x15 && (((iVar10 < 0x1a && (iVar4 < 0x2b)) && (iVar12 == 1)))))) ||
               (((iVar7 < iVar8 && (iVar3 < 0xc2)) &&
                 ((iVar1 < 0xd7 &&
                   (((iVar2 < 0x11 && (iVar10 < 0x1a)) && ((iVar4 < 0x2d && (iVar5 == 1)))))))))))))))))) ||
      (((((((((((iVar14 = iVar17 * 0x59, iVar14 < iVar8 && (param_2[1] < 0xf)) && (iVar3 < 0xc2)) &&
               ((iVar1 < 0xd7 && (iVar2 < 0x1a)))) && (iVar10 < 0x1a)) && (iVar4 < 0x2e)) ||
            (((iVar12 = iVar17 * 0x58, iVar12 < iVar8 && (param_2[1] < 0xd)) &&
              ((iVar3 < 0xbd &&
                ((((iVar1 < 0xd8 && (iVar2 < 0x10)) && (iVar10 < 0x15)) && (iVar4 < 0x33)))))))) ||
           ((iVar14 < iVar8 &&
             ((((iVar3 < 0xc0 && (iVar1 < 0xc9)) && ((iVar2 < 0x3a && (iVar10 < 0x19)))) ||
               ((iVar14 < iVar8 &&
                 ((((iVar3 < 0xc3 && (iVar1 < 0xd3)) &&
                    ((iVar10 < 0x2d && ((iVar2 < 0x33 && (param_2[4] < 0xf0)))))) ||
                   (((iVar14 < iVar8 && (((iVar3 < 0xc3 && (iVar1 < 199)) && (iVar10 < 0x14)))) &&
                     ((iVar2 < 0x42 && (param_2[4] < 0xe3)))))))))))))) ||
          (((iVar12 < iVar8 &&
             ((((iVar3 < 0xd6 && (iVar1 < 0xcb)) &&
                ((iVar10 < 0xf && ((iVar2 < 0x17 && (param_2[4] < 0xf0)))))) ||
               ((iVar12 < iVar8 &&
                 ((((iVar3 < 0xbd && (iVar1 < 0xc1)) && (iVar10 < 0x16)) &&
                   ((iVar2 < 0x3d && (param_2[4] < 0xdf)))))))))) ||
            ((((iVar17 * 0x57 < iVar8 && ((iVar18 < 0x18d && (iVar10 < 0x16)))) && (iVar2 < 0x33)) &&
              (param_2[4] < 0xe4)))))) ||
         ((((iVar17 * 0x56 < iVar8 && (iVar18 < 0x182)) && (iVar10 < 0x1b)) &&
           ((iVar2 < 0x44 && (param_2[4] < 0xde)))))) ||
        (((((iVar17 * 0x55 < iVar8 && ((iVar3 < 0xb1 && (iVar1 < 0xbf)))) && (iVar10 < 0x16)) &&
           ((iVar2 < 0x2e && (param_2[4] < 0xd0)))) ||
          ((iVar17 * 0x53 < iVar8 &&
            (((((iVar18 < 0x191 && (iVar10 < 0x16)) && (iVar2 < 0x2b)) && (param_2[4] < 0xe0)) ||
              ((((iVar17 * 0x53 < iVar8 && (iVar18 < 0x1af)) &&
                 ((iVar2 < 0x10 && ((iVar10 < 0x19 && (iVar4 < 0x24)))))) && (iVar16 < 0x45))))))))))))
    bVar6 = TRUE;
LAB_18002fa16:
  if (((((iVar17 * 0x5c < iVar8) && (iVar9 = *param_2, iVar9 < 0x12)) && (iVar10 < 0x1a)) &&
       (iVar3 < 0xda)) ||
      (((iVar17 * 0x5d < iVar8 && (iVar9 < 10)) && ((iVar10 < 0xd && (iVar3 < 0xdc))))))
    *param_7 = 0;
  if (bVar6)
    *param_6 = *param_6 + 1;
  return bVar6;
}

gboolean
goodix_chicago_match_late_rejection_compat (
  guint                                template_type,
  guint                                width,
  guint                                height,
  gint32                               probe_quality,
  const GoodixChicagoMatchScoreRecord *record,
  const gint32                         transform[6],
  gint32                               current_auxiliary,
  gint32                               candidate_auxiliary,
  gint32                               combined_auxiliary,
  gint32                              *rejection_count,
  gint32                              *status_flag)
{
  gint32 probe[0x44] = { 0, };
  gint32 state[3] = {
    current_auxiliary,
    candidate_auxiliary,
    combined_auxiliary,
  };

  g_return_val_if_fail (
    template_type == 7 || template_type == 10 ||
    template_type == 23 || template_type == 25 || template_type == 26,
    FALSE);
  g_return_val_if_fail (width > 0 && width <= G_MAXINT32, FALSE);
  g_return_val_if_fail (height > 0 && height <= G_MAXINT32, FALSE);
  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (transform != NULL, FALSE);
  g_return_val_if_fail (rejection_count != NULL, FALSE);
  g_return_val_if_fail (status_flag != NULL, FALSE);

  probe[0] = width;
  probe[1] = height;
  probe[0x43] = probe_quality;
  return match_late_rejection_policy_compat (
    probe, (gint32 *) record, template_type, (gint32 *) transform, state,
    rejection_count, status_flag);
}
