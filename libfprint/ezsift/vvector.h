/*
 * Matrix macros used by ezSIFT's keypoint refinement.
 *
 * Extracted verbatim from ezSIFT's original vvector.h (a large macro
 * library of which only these three macros are used here). See
 * https://github.com/robertwgh/ezSIFT
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EZSIFT_VVECTOR_H
#define EZSIFT_VVECTOR_H

#define MAT_DOT_VEC_3X3(p, m, v)                                               \
                                                                               \
    {                                                                          \
        p[0] = m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2];               \
        p[1] = m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2];               \
        p[2] = m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2];               \
    }

#define DETERMINANT_3X3(d, m)                                                  \
                                                                               \
    {                                                                          \
        d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]);                 \
        d -= m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]);                \
        d += m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);                \
    }

#define SCALE_ADJOINT_3X3(a, s, m)                                             \
                                                                               \
    {                                                                          \
        a[0][0] = (s) * (m[1][1] * m[2][2] - m[1][2] * m[2][1]);               \
        a[1][0] = (s) * (m[1][2] * m[2][0] - m[1][0] * m[2][2]);               \
        a[2][0] = (s) * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);               \
                                                                               \
        a[0][1] = (s) * (m[0][2] * m[2][1] - m[0][1] * m[2][2]);               \
        a[1][1] = (s) * (m[0][0] * m[2][2] - m[0][2] * m[2][0]);               \
        a[2][1] = (s) * (m[0][1] * m[2][0] - m[0][0] * m[2][1]);               \
                                                                               \
        a[0][2] = (s) * (m[0][1] * m[1][2] - m[0][2] * m[1][1]);               \
        a[1][2] = (s) * (m[0][2] * m[1][0] - m[0][0] * m[1][2]);               \
        a[2][2] = (s) * (m[0][0] * m[1][1] - m[0][1] * m[1][0]);               \
    }

#endif