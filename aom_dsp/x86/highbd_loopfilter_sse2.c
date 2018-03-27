/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <emmintrin.h>  // SSE2

#include "./aom_dsp_rtcd.h"
#include "aom_dsp/x86/lpf_common_sse2.h"
#include "aom_ports/emmintrin_compat.h"
#include "aom_ports/mem.h"

static AOM_FORCE_INLINE void pixel_clamp(const __m128i *min, const __m128i *max,
                                         __m128i *pixel) {
  *pixel = _mm_min_epi16(*pixel, *max);
  *pixel = _mm_max_epi16(*pixel, *min);
}

static AOM_FORCE_INLINE __m128i abs_diff16(__m128i a, __m128i b) {
  return _mm_or_si128(_mm_subs_epu16(a, b), _mm_subs_epu16(b, a));
}

static INLINE void get_limit(const uint8_t *bl, const uint8_t *l,
                             const uint8_t *t, int bd, __m128i *blt,
                             __m128i *lt, __m128i *thr) {
  const int shift = bd - 8;
  const __m128i zero = _mm_setzero_si128();

  __m128i x = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)bl), zero);
  *blt = _mm_slli_epi16(x, shift);

  x = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)l), zero);
  *lt = _mm_slli_epi16(x, shift);

  x = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)t), zero);
  *thr = _mm_slli_epi16(x, shift);
}

static INLINE void load_highbd_pixel(const uint16_t *s, int size, int pitch,
                                     __m128i *p, __m128i *q) {
  int i;
  for (i = 0; i < size; i++) {
    p[i] = _mm_loadu_si128((__m128i *)(s - (i + 1) * pitch));
    q[i] = _mm_loadu_si128((__m128i *)(s + i * pitch));
  }
}

static INLINE void highbd_hev_mask(const __m128i *p0q0, const __m128i *p1q1,
                                   const __m128i *t, __m128i *abs_p1p0,
                                   __m128i *hev) {
  *abs_p1p0 = abs_diff16(*p1q1, *p0q0);
  __m128i abs_q1q0 = _mm_srli_si128(*abs_p1p0, 8);
  __m128i h = _mm_max_epi16(*abs_p1p0, abs_q1q0);
  h = _mm_subs_epu16(h, *t);

  const __m128i ffff = _mm_set1_epi16(0xFFFF);
  const __m128i zero = _mm_setzero_si128();
  *hev = _mm_xor_si128(_mm_cmpeq_epi16(h, zero), ffff);
  // replicate for the further "merged variables" usage
  *hev = _mm_unpacklo_epi64(*hev, *hev);
}

static INLINE void highbd_filter_mask(const __m128i *p, const __m128i *q,
                                      const __m128i *l, const __m128i *bl,
                                      __m128i *mask) {
  __m128i abs_p0q0 = abs_diff16(p[0], q[0]);
  __m128i abs_p1q1 = abs_diff16(p[1], q[1]);
  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);

  const __m128i zero = _mm_setzero_si128();
  const __m128i one = _mm_set1_epi16(1);
  const __m128i ffff = _mm_set1_epi16(0xFFFF);
  __m128i max = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), *bl);
  max = _mm_xor_si128(_mm_cmpeq_epi16(max, zero), ffff);
  max = _mm_and_si128(max, _mm_adds_epu16(*l, one));

  int i;
  for (i = 1; i < 4; ++i) {
    max = _mm_max_epi16(max, abs_diff16(p[i], p[i - 1]));
    max = _mm_max_epi16(max, abs_diff16(q[i], q[i - 1]));
  }
  max = _mm_subs_epu16(max, *l);
  *mask = _mm_cmpeq_epi16(max, zero);  // return ~mask
}

static INLINE void flat_mask_internal(const __m128i *th, const __m128i *p,
                                      const __m128i *q, int bd, int start,
                                      int end, __m128i *flat) {
  __m128i max = _mm_setzero_si128();
  int i;
  for (i = start; i < end; ++i) {
    max = _mm_max_epi16(max, abs_diff16(p[i], p[0]));
    max = _mm_max_epi16(max, abs_diff16(q[i], q[0]));
  }

  __m128i ft;
  if (bd == 8)
    ft = _mm_subs_epu16(max, *th);
  else if (bd == 10)
    ft = _mm_subs_epu16(max, _mm_slli_epi16(*th, 2));
  else  // bd == 12
    ft = _mm_subs_epu16(max, _mm_slli_epi16(*th, 4));

  const __m128i zero = _mm_setzero_si128();
  *flat = _mm_cmpeq_epi16(ft, zero);
}

// Note:
//  Access p[3-1], p[0], and q[3-1], q[0]
static INLINE void highbd_flat_mask4(const __m128i *th, const __m128i *p,
                                     const __m128i *q, __m128i *flat, int bd) {
  // check the distance 1,2,3 against 0
  flat_mask_internal(th, p, q, bd, 1, 4, flat);
}

// Note:
//  access p[6-4], p[0], and q[6-4], q[0]
static INLINE void highbd_flat_mask4_13(const __m128i *th, const __m128i *p,
                                        const __m128i *q, __m128i *flat,
                                        int bd) {
  flat_mask_internal(th, p, q, bd, 4, 7, flat);
}

// Note:
//  access p[7-4], p[0], and q[7-4], q[0]
static INLINE void highbd_flat_mask5(const __m128i *th, const __m128i *p,
                                     const __m128i *q, __m128i *flat, int bd) {
  flat_mask_internal(th, p, q, bd, 4, 8, flat);
}

static AOM_FORCE_INLINE void highbd_filter4_sse2(__m128i *p1p0, __m128i *q1q0,
                                                 __m128i *hev, __m128i *mask,
                                                 __m128i *qs1qs0,
                                                 __m128i *ps1ps0, __m128i *t80,
                                                 int bd) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i one = _mm_set1_epi16(1);
  const __m128i pmax =
      _mm_subs_epi16(_mm_subs_epi16(_mm_slli_epi16(one, bd), one), *t80);
  const __m128i pmin = _mm_subs_epi16(zero, *t80);

  const __m128i t3t4 = _mm_set_epi16(3, 3, 3, 3, 4, 4, 4, 4);
  __m128i ps1ps0_work, qs1qs0_work, work;
  __m128i filt, filter2filter1, filter2filt, filter1filt;

  ps1ps0_work = _mm_subs_epi16(*p1p0, *t80);
  qs1qs0_work = _mm_subs_epi16(*q1q0, *t80);

  work = _mm_subs_epi16(ps1ps0_work, qs1qs0_work);
  pixel_clamp(&pmin, &pmax, &work);
  filt = _mm_and_si128(_mm_srli_si128(work, 8), *hev);

  filt = _mm_subs_epi16(filt, work);
  filt = _mm_subs_epi16(filt, work);
  filt = _mm_subs_epi16(filt, work);
  // (aom_filter + 3 * (qs0 - ps0)) & mask
  pixel_clamp(&pmin, &pmax, &filt);
  filt = _mm_and_si128(filt, *mask);
  filt = _mm_unpacklo_epi64(filt, filt);

  filter2filter1 = _mm_adds_epi16(filt, t3t4); /* signed_short_clamp */
  pixel_clamp(&pmin, &pmax, &filter2filter1);
  filter2filter1 = _mm_srai_epi16(filter2filter1, 3); /* >> 3 */

  filt = _mm_unpacklo_epi64(filter2filter1, filter2filter1);

  // filt >> 1
  filt = _mm_adds_epi16(filt, one);
  filt = _mm_srai_epi16(filt, 1);
  filt = _mm_andnot_si128(*hev, filt);

  filter2filt = _mm_unpackhi_epi64(filter2filter1, filt);
  filter1filt = _mm_unpacklo_epi64(filter2filter1, filt);

  qs1qs0_work = _mm_subs_epi16(qs1qs0_work, filter1filt);
  ps1ps0_work = _mm_adds_epi16(ps1ps0_work, filter2filt);

  pixel_clamp(&pmin, &pmax, &qs1qs0_work);
  pixel_clamp(&pmin, &pmax, &ps1ps0_work);

  *qs1qs0 = _mm_adds_epi16(qs1qs0_work, *t80);
  *ps1ps0 = _mm_adds_epi16(ps1ps0_work, *t80);
}

static INLINE void highbd_filter4_dual_sse2(__m128i *p, __m128i *q,
                                            const __m128i *mask,
                                            const __m128i *th, int bd,
                                            __m128i *ps, __m128i *qs) {
  __m128i t80;
  if (bd == 8)
    t80 = _mm_set1_epi16(0x80);
  else if (bd == 10)
    t80 = _mm_set1_epi16(0x200);
  else  // bd == 12
    t80 = _mm_set1_epi16(0x800);
  __m128i ps0 = _mm_subs_epi16(p[0], t80);
  __m128i ps1 = _mm_subs_epi16(p[1], t80);
  __m128i qs0 = _mm_subs_epi16(q[0], t80);
  __m128i qs1 = _mm_subs_epi16(q[1], t80);
  const __m128i one = _mm_set1_epi16(1);
  const __m128i pmax =
      _mm_subs_epi16(_mm_subs_epi16(_mm_slli_epi16(one, bd), one), t80);
  const __m128i zero = _mm_setzero_si128();
  const __m128i pmin = _mm_subs_epi16(zero, t80);
  __m128i filter = _mm_subs_epi16(ps1, qs1);
  pixel_clamp(&pmin, &pmax, &filter);

  // highbd_hev_mask(p, q, th, &hev);
  __m128i hev;
  const __m128i abs_p1p0 =
      _mm_or_si128(_mm_subs_epu16(p[1], p[0]), _mm_subs_epu16(p[0], p[1]));
  const __m128i abs_q1q0 =
      _mm_or_si128(_mm_subs_epu16(q[1], q[0]), _mm_subs_epu16(q[0], q[1]));
  __m128i h = _mm_max_epi16(abs_p1p0, abs_q1q0);
  h = _mm_subs_epu16(h, *th);
  const __m128i ffff = _mm_set1_epi16(0xFFFF);
  hev = _mm_xor_si128(_mm_cmpeq_epi16(h, zero), ffff);
  filter = _mm_and_si128(filter, hev);

  const __m128i x = _mm_subs_epi16(qs0, ps0);
  filter = _mm_adds_epi16(filter, x);
  filter = _mm_adds_epi16(filter, x);
  filter = _mm_adds_epi16(filter, x);
  pixel_clamp(&pmin, &pmax, &filter);
  filter = _mm_and_si128(filter, *mask);
  const __m128i t3 = _mm_set1_epi16(3);
  const __m128i t4 = _mm_set1_epi16(4);
  __m128i filter1 = _mm_adds_epi16(filter, t4);
  __m128i filter2 = _mm_adds_epi16(filter, t3);
  pixel_clamp(&pmin, &pmax, &filter1);
  pixel_clamp(&pmin, &pmax, &filter2);
  filter1 = _mm_srai_epi16(filter1, 3);
  filter2 = _mm_srai_epi16(filter2, 3);
  qs0 = _mm_subs_epi16(qs0, filter1);
  pixel_clamp(&pmin, &pmax, &qs0);
  ps0 = _mm_adds_epi16(ps0, filter2);
  pixel_clamp(&pmin, &pmax, &ps0);
  qs[0] = _mm_adds_epi16(qs0, t80);
  ps[0] = _mm_adds_epi16(ps0, t80);
  filter = _mm_adds_epi16(filter1, one);
  filter = _mm_srai_epi16(filter, 1);
  filter = _mm_andnot_si128(hev, filter);
  qs1 = _mm_subs_epi16(qs1, filter);
  pixel_clamp(&pmin, &pmax, &qs1);
  ps1 = _mm_adds_epi16(ps1, filter);
  pixel_clamp(&pmin, &pmax, &ps1);
  qs[1] = _mm_adds_epi16(qs1, t80);
  ps[1] = _mm_adds_epi16(ps1, t80);
}

static INLINE void highbd_lpf_horz_edge_8_sse2(uint16_t *s, int pitch,
                                               const uint8_t *blt,
                                               const uint8_t *lt,
                                               const uint8_t *thr, int bd) {
  int i;
  __m128i blimit, limit, thresh;
  get_limit(blt, lt, thr, bd, &blimit, &limit, &thresh);

  __m128i p[7], q[7], pq[7];
  load_highbd_pixel(s, 7, pitch, p, q);

  __m128i mask;
  highbd_filter_mask(p, q, &limit, &blimit, &mask);

  __m128i flat, flat2;
  const __m128i one = _mm_set1_epi16(1);
  highbd_flat_mask4(&one, p, q, &flat, bd);
  highbd_flat_mask4_13(&one, p, q, &flat2, bd);

  flat = _mm_and_si128(flat, mask);
  flat2 = _mm_and_si128(flat2, flat);

  // replicate for the further "merged variables" usage
  flat = _mm_unpacklo_epi64(flat, flat);
  flat2 = _mm_unpacklo_epi64(flat2, flat2);

  __m128i ps0ps1, qs0qs1, p1p0, q1q0;
  __m128i t80;

  if (bd == 8)
    t80 = _mm_set1_epi16(0x80);
  else if (bd == 10)
    t80 = _mm_set1_epi16(0x200);
  else  // bd == 12
    t80 = _mm_set1_epi16(0x800);

  // filters - hev and filter4
  __m128i hevhev;
  __m128i abs_p1p0;
  for (i = 0; i < 6; i++) {
    pq[i] = _mm_unpacklo_epi64(p[i], q[i]);
  }

  highbd_hev_mask(&pq[0], &pq[1], &thresh, &abs_p1p0, &hevhev);

  p1p0 = _mm_unpacklo_epi64(p[0], p[1]);
  q1q0 = _mm_unpacklo_epi64(q[0], q[1]);
  highbd_filter4_sse2(&p1p0, &q1q0, &hevhev, &mask, &qs0qs1, &ps0ps1, &t80, bd);

  // flat and wide flat calculations
  __m128i flat_p[3], flat_q[3], flat_pq[3];
  __m128i flat2_p[6], flat2_q[6];
  __m128i flat2_pq[6];

  {
    const __m128i eight = _mm_set1_epi16(8);
    const __m128i four = _mm_set1_epi16(4);
    __m128i sum_p = _mm_add_epi16(p[5], _mm_add_epi16(p[4], p[3]));
    __m128i sum_q = _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[3]));

    __m128i sum_lp = _mm_add_epi16(p[0], _mm_add_epi16(p[2], p[1]));
    sum_p = _mm_add_epi16(sum_p, sum_lp);

    __m128i sum_lq = _mm_add_epi16(q[0], _mm_add_epi16(q[2], q[1]));
    sum_q = _mm_add_epi16(sum_q, sum_lq);

    sum_p = _mm_add_epi16(eight, _mm_add_epi16(sum_p, sum_q));
    sum_lp = _mm_add_epi16(four, _mm_add_epi16(sum_lp, sum_lq));

    flat2_p[0] = _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(p[6], p[0]),
                                                    _mm_add_epi16(p[1], q[0])));
    flat2_q[0] = _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(q[6], q[0]),
                                                    _mm_add_epi16(p[0], q[1])));

    flat_p[0] = _mm_add_epi16(sum_lp, _mm_add_epi16(p[3], p[0]));
    flat_q[0] = _mm_add_epi16(sum_lp, _mm_add_epi16(q[3], q[0]));
    __m128i sum_p6 = _mm_add_epi16(p[6], p[6]);
    __m128i sum_q6 = _mm_add_epi16(q[6], q[6]);
    __m128i sum_p3 = _mm_add_epi16(p[3], p[3]);
    __m128i sum_q3 = _mm_add_epi16(q[3], q[3]);

    sum_q = _mm_sub_epi16(sum_p, p[5]);
    sum_p = _mm_sub_epi16(sum_p, q[5]);

    flat2_p[1] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[1], _mm_add_epi16(p[2], p[0]))));
    flat2_q[1] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[1], _mm_add_epi16(q[0], q[2]))));

    sum_lq = _mm_sub_epi16(sum_lp, p[2]);
    sum_lp = _mm_sub_epi16(sum_lp, q[2]);

    flat_p[1] = _mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[1]));
    flat_q[1] = _mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[1]));

    flat_pq[0] = _mm_srli_epi16(_mm_unpacklo_epi64(flat_p[0], flat_q[0]), 3);
    flat_pq[1] = _mm_srli_epi16(_mm_unpacklo_epi64(flat_p[1], flat_q[1]), 3);

    flat2_pq[0] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[0], flat2_q[0]), 4);
    flat2_pq[1] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[1], flat2_q[1]), 4);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p3 = _mm_add_epi16(sum_p3, p[3]);
    sum_q3 = _mm_add_epi16(sum_q3, q[3]);
    sum_p = _mm_sub_epi16(sum_p, q[4]);
    sum_q = _mm_sub_epi16(sum_q, p[4]);
    flat2_p[2] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[2], _mm_add_epi16(p[3], p[1]))));
    flat2_q[2] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[2], _mm_add_epi16(q[1], q[3]))));
    flat2_pq[2] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[2], flat2_q[2]), 4);

    sum_lp = _mm_sub_epi16(sum_lp, q[1]);
    sum_lq = _mm_sub_epi16(sum_lq, p[1]);
    flat_p[2] = _mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[2]));
    flat_q[2] = _mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[2]));
    flat_pq[2] = _mm_srli_epi16(_mm_unpacklo_epi64(flat_p[2], flat_q[2]), 3);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[3]);
    sum_q = _mm_sub_epi16(sum_q, p[3]);
    flat2_p[3] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[3], _mm_add_epi16(p[4], p[2]))));
    flat2_q[3] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[3], _mm_add_epi16(q[2], q[4]))));
    flat2_pq[3] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[3], flat2_q[3]), 4);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[2]);
    sum_q = _mm_sub_epi16(sum_q, p[2]);
    flat2_p[4] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[4], _mm_add_epi16(p[5], p[3]))));
    flat2_q[4] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[4], _mm_add_epi16(q[3], q[5]))));
    flat2_pq[4] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[4], flat2_q[4]), 4);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[1]);
    sum_q = _mm_sub_epi16(sum_q, p[1]);
    flat2_p[5] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[5], _mm_add_epi16(p[6], p[4]))));
    flat2_q[5] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[6]))));

    flat2_pq[5] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[5], flat2_q[5]), 4);
  }

  // highbd_filter8
  pq[0] = _mm_unpacklo_epi64(ps0ps1, qs0qs1);
  pq[1] = _mm_unpackhi_epi64(ps0ps1, qs0qs1);

  for (i = 0; i < 3; i++) {
    pq[i] = _mm_andnot_si128(flat, pq[i]);
    flat_pq[i] = _mm_and_si128(flat, flat_pq[i]);
    pq[i] = _mm_or_si128(pq[i], flat_pq[i]);
  }

  // highbd_filter16
  for (i = 5; i >= 0; i--) {
    //  p[i] remains unchanged if !(flat2 && flat && mask)
    pq[i] = _mm_andnot_si128(flat2, pq[i]);
    flat2_pq[i] = _mm_and_si128(flat2, flat2_pq[i]);
    //  get values for when (flat2 && flat && mask)
    pq[i] = _mm_or_si128(pq[i], flat2_pq[i]);  // full list of pq values

    _mm_storel_epi64((__m128i *)(s - (i + 1) * pitch), pq[i]);
    _mm_storel_epi64((__m128i *)(s + i * pitch), _mm_srli_si128(pq[i], 8));
  }
}

static INLINE void highbd_lpf_horz_edge_8_dual_sse2(uint16_t *s, int pitch,
                                                    const uint8_t *blt,
                                                    const uint8_t *lt,
                                                    const uint8_t *thr,
                                                    int bd) {
  __m128i blimit, limit, thresh;
  get_limit(blt, lt, thr, bd, &blimit, &limit, &thresh);
  __m128i p[7], q[7];
  load_highbd_pixel(s, 7, pitch, p, q);
  __m128i mask;
  highbd_filter_mask(p, q, &limit, &blimit, &mask);
  __m128i flat, flat2;
  const __m128i one = _mm_set1_epi16(1);
  highbd_flat_mask4(&one, p, q, &flat, bd);
  highbd_flat_mask4_13(&one, p, q, &flat2, bd);
  flat = _mm_and_si128(flat, mask);
  flat2 = _mm_and_si128(flat2, flat);
  __m128i ps[2], qs[2];
  highbd_filter4_dual_sse2(p, q, &mask, &thresh, bd, ps, qs);
  // flat and wide flat calculations
  __m128i flat_p[3], flat_q[3];
  __m128i flat2_p[6], flat2_q[6];
  {
    const __m128i eight = _mm_set1_epi16(8);
    const __m128i four = _mm_set1_epi16(4);
    __m128i sum_p = _mm_add_epi16(p[5], _mm_add_epi16(p[4], p[3]));
    __m128i sum_q = _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[3]));
    __m128i sum_lp = _mm_add_epi16(p[0], _mm_add_epi16(p[2], p[1]));
    sum_p = _mm_add_epi16(sum_p, sum_lp);
    __m128i sum_lq = _mm_add_epi16(q[0], _mm_add_epi16(q[2], q[1]));
    sum_q = _mm_add_epi16(sum_q, sum_lq);
    sum_p = _mm_add_epi16(eight, _mm_add_epi16(sum_p, sum_q));
    sum_lp = _mm_add_epi16(four, _mm_add_epi16(sum_lp, sum_lq));
    flat2_p[0] = _mm_srli_epi16(
        _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(p[6], p[0]),
                                           _mm_add_epi16(p[1], q[0]))),
        4);
    flat2_q[0] = _mm_srli_epi16(
        _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(q[6], q[0]),
                                           _mm_add_epi16(p[0], q[1]))),
        4);
    flat_p[0] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(p[3], p[0])), 3);
    flat_q[0] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(q[3], q[0])), 3);
    __m128i sum_p6 = _mm_add_epi16(p[6], p[6]);
    __m128i sum_q6 = _mm_add_epi16(q[6], q[6]);
    __m128i sum_p3 = _mm_add_epi16(p[3], p[3]);
    __m128i sum_q3 = _mm_add_epi16(q[3], q[3]);
    sum_q = _mm_sub_epi16(sum_p, p[5]);
    sum_p = _mm_sub_epi16(sum_p, q[5]);
    flat2_p[1] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[1], _mm_add_epi16(p[2], p[0])))),
        4);
    flat2_q[1] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[1], _mm_add_epi16(q[0], q[2])))),
        4);
    sum_lq = _mm_sub_epi16(sum_lp, p[2]);
    sum_lp = _mm_sub_epi16(sum_lp, q[2]);
    flat_p[1] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[1])), 3);
    flat_q[1] =
        _mm_srli_epi16(_mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[1])), 3);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p3 = _mm_add_epi16(sum_p3, p[3]);
    sum_q3 = _mm_add_epi16(sum_q3, q[3]);
    sum_p = _mm_sub_epi16(sum_p, q[4]);
    sum_q = _mm_sub_epi16(sum_q, p[4]);
    flat2_p[2] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[2], _mm_add_epi16(p[3], p[1])))),
        4);
    flat2_q[2] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[2], _mm_add_epi16(q[1], q[3])))),
        4);
    sum_lp = _mm_sub_epi16(sum_lp, q[1]);
    sum_lq = _mm_sub_epi16(sum_lq, p[1]);
    flat_p[2] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[2])), 3);
    flat_q[2] =
        _mm_srli_epi16(_mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[2])), 3);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[3]);
    sum_q = _mm_sub_epi16(sum_q, p[3]);
    flat2_p[3] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[3], _mm_add_epi16(p[4], p[2])))),
        4);
    flat2_q[3] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[3], _mm_add_epi16(q[2], q[4])))),
        4);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[2]);
    sum_q = _mm_sub_epi16(sum_q, p[2]);
    flat2_p[4] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[4], _mm_add_epi16(p[5], p[3])))),
        4);
    flat2_q[4] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[4], _mm_add_epi16(q[3], q[5])))),
        4);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[1]);
    sum_q = _mm_sub_epi16(sum_q, p[1]);
    flat2_p[5] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[5], _mm_add_epi16(p[6], p[4])))),
        4);
    flat2_q[5] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[6])))),
        4);
  }
  // highbd_filter8
  p[2] = _mm_andnot_si128(flat, p[2]);
  //  p2 remains unchanged if !(flat && mask)
  flat_p[2] = _mm_and_si128(flat, flat_p[2]);
  //  when (flat && mask)
  p[2] = _mm_or_si128(p[2], flat_p[2]);  // full list of p2 values
  q[2] = _mm_andnot_si128(flat, q[2]);
  flat_q[2] = _mm_and_si128(flat, flat_q[2]);
  q[2] = _mm_or_si128(q[2], flat_q[2]);  // full list of q2 values
  int i;
  for (i = 1; i >= 0; i--) {
    ps[i] = _mm_andnot_si128(flat, ps[i]);
    flat_p[i] = _mm_and_si128(flat, flat_p[i]);
    p[i] = _mm_or_si128(ps[i], flat_p[i]);
    qs[i] = _mm_andnot_si128(flat, qs[i]);
    flat_q[i] = _mm_and_si128(flat, flat_q[i]);
    q[i] = _mm_or_si128(qs[i], flat_q[i]);
  }
  // highbd_filter16
  for (i = 5; i >= 0; i--) {
    //  p[i] remains unchanged if !(flat2 && flat && mask)
    p[i] = _mm_andnot_si128(flat2, p[i]);
    flat2_p[i] = _mm_and_si128(flat2, flat2_p[i]);
    //  get values for when (flat2 && flat && mask)
    p[i] = _mm_or_si128(p[i], flat2_p[i]);  // full list of p values
    q[i] = _mm_andnot_si128(flat2, q[i]);
    flat2_q[i] = _mm_and_si128(flat2, flat2_q[i]);
    q[i] = _mm_or_si128(q[i], flat2_q[i]);
    _mm_store_si128((__m128i *)(s - (i + 1) * pitch), p[i]);
    _mm_store_si128((__m128i *)(s + i * pitch), q[i]);
  }
}

void aom_highbd_lpf_horizontal_14_sse2(uint16_t *s, int p,
                                       const uint8_t *_blimit,
                                       const uint8_t *_limit,
                                       const uint8_t *_thresh, int bd) {
  highbd_lpf_horz_edge_8_sse2(s, p, _blimit, _limit, _thresh, bd);
}

void aom_highbd_lpf_horizontal_14_dual_sse2(uint16_t *s, int p,
                                            const uint8_t *_blimit,
                                            const uint8_t *_limit,
                                            const uint8_t *_thresh, int bd) {
  highbd_lpf_horz_edge_8_sse2(s, p, _blimit, _limit, _thresh, bd);
  highbd_lpf_horz_edge_8_sse2(s + 4, p, _blimit, _limit, _thresh, bd);
}

static INLINE void store_horizontal_8(const __m128i *p2, const __m128i *p1,
                                      const __m128i *p0, const __m128i *q0,
                                      const __m128i *q1, const __m128i *q2,
                                      int p, uint16_t *s) {
  _mm_storel_epi64((__m128i *)(s - 3 * p), *p2);
  _mm_storel_epi64((__m128i *)(s - 2 * p), *p1);
  _mm_storel_epi64((__m128i *)(s - 1 * p), *p0);
  _mm_storel_epi64((__m128i *)(s + 0 * p), *q0);
  _mm_storel_epi64((__m128i *)(s + 1 * p), *q1);
  _mm_storel_epi64((__m128i *)(s + 2 * p), *q2);
}

void aom_highbd_lpf_horizontal_6_sse2(uint16_t *s, int p,
                                      const uint8_t *_blimit,
                                      const uint8_t *_limit,
                                      const uint8_t *_thresh, int bd) {
  const __m128i zero = _mm_setzero_si128();
  __m128i blimit, limit, thresh;
  __m128i mask, hev, flat;
  __m128i p2, p1, p0, q0, q1, q2;
  __m128i q2p2, q1p1, q0p0, p1q1, p0q0;
  __m128i p1p0, q1q0, ps1ps0, qs1qs0;
  __m128i flat_p1p0, flat_q0q1;

  p2 = _mm_loadl_epi64((__m128i *)(s - 3 * p));
  p1 = _mm_loadl_epi64((__m128i *)(s - 2 * p));
  p0 = _mm_loadl_epi64((__m128i *)(s - 1 * p));
  q0 = _mm_loadl_epi64((__m128i *)(s + 0 * p));
  q1 = _mm_loadl_epi64((__m128i *)(s + 1 * p));
  q2 = _mm_loadl_epi64((__m128i *)(s + 2 * p));

  q2p2 = _mm_unpacklo_epi64(p2, q2);
  q1p1 = _mm_unpacklo_epi64(p1, q1);
  q0p0 = _mm_unpacklo_epi64(p0, q0);

  p1q1 = _mm_shuffle_epi32(q1p1, _MM_SHUFFLE(1, 0, 3, 2));
  p0q0 = _mm_shuffle_epi32(q0p0, _MM_SHUFFLE(1, 0, 3, 2));

  __m128i abs_p1q1, abs_p0q0, abs_p1p0, work;

  const __m128i four = _mm_set1_epi16(4);
  __m128i t80;
  const __m128i one = _mm_set1_epi16(0x1);
  const __m128i ffff = _mm_cmpeq_epi16(one, one);

  if (bd == 8) {
    blimit = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero);
    limit = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero);
    thresh = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero);
    t80 = _mm_set1_epi16(0x80);
  } else if (bd == 10) {
    blimit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero), 2);
    limit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero), 2);
    thresh = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero), 2);
    t80 = _mm_set1_epi16(0x200);
  } else {  // bd == 12
    blimit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero), 4);
    limit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero), 4);
    thresh = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero), 4);
    t80 = _mm_set1_epi16(0x800);
  }

  // filter_mask and hev_mask
  highbd_hev_mask(&p0q0, &p1q1, &thresh, &abs_p1p0, &hev);

  abs_p0q0 = abs_diff16(q0p0, p0q0);
  abs_p1q1 = abs_diff16(q1p1, p1q1);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(p0 - q0) * 2 + abs(p1 - q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit, one));
  mask = _mm_max_epi16(abs_p1p0, mask);
  // mask |= (abs(p1 - p0) > limit) * -1;
  // mask |= (abs(q1 - q0) > limit) * -1;

  work = abs_diff16(q2p2, q1p1);

  mask = _mm_max_epi16(work, mask);
  mask = _mm_max_epi16(mask, _mm_srli_si128(mask, 8));
  mask = _mm_subs_epu16(mask, limit);
  mask = _mm_cmpeq_epi16(mask, zero);

  // flat_mask
  flat = _mm_max_epi16(abs_diff16(q2p2, q0p0), abs_p1p0);
  flat = _mm_max_epi16(flat, _mm_srli_si128(flat, 8));

  if (bd == 8)
    flat = _mm_subs_epu16(flat, one);
  else if (bd == 10)
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 2));
  else  // bd == 12
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 4));

  flat = _mm_cmpeq_epi16(flat, zero);
  flat = _mm_and_si128(
      flat, mask);  // flat & mask
                    // replicate for the further "merged variables" usage
  flat = _mm_unpacklo_epi64(flat, flat);

  {
    __m128i workp_a, workp_b, workp_shft0, workp_shft1;

    // op1
    workp_a = _mm_add_epi16(_mm_add_epi16(p0, p0),
                            _mm_add_epi16(p1, p1));  // p0 *2 + p1 * 2
    workp_a = _mm_add_epi16(_mm_add_epi16(workp_a, four),
                            p2);  // p2 + p0 * 2 + p1 * 2 + 4

    workp_b = _mm_add_epi16(_mm_add_epi16(p2, p2), q0);
    workp_shft0 =
        _mm_add_epi16(workp_a, workp_b);  // p2 * 3 + p1 * 2 + p0 * 2 + q0 + 4

    // op0
    workp_b = _mm_add_epi16(_mm_add_epi16(q0, q0), q1);  // q0 * 2 + q1
    workp_a = _mm_add_epi16(workp_a,
                            workp_b);  // p2 + p0 * 2 + p1 * 2 + q0 * 2 + q1 + 4

    flat_p1p0 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_a, workp_shft0), 3);

    // oq0
    workp_a = _mm_sub_epi16(_mm_sub_epi16(workp_a, p2),
                            p1);  // p0 * 2 + p1  + q0 * 2 + q1 + 4
    workp_b = _mm_add_epi16(q1, q2);
    workp_shft0 = _mm_add_epi16(
        workp_a, workp_b);  // p0 * 2 + p1  + q0 * 2 + q1 * 2 + q2 + 4

    // oq1
    workp_a = _mm_sub_epi16(_mm_sub_epi16(workp_shft0, p1),
                            p0);  // p0   + q0 * 2 + q1 * 2 + q2 + 4
    workp_b = _mm_add_epi16(q2, q2);
    workp_shft1 =
        _mm_add_epi16(workp_a, workp_b);  // p0  + q0 * 2 + q1 * 2 + q2 * 3 + 4

    flat_q0q1 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_shft0, workp_shft1), 3);
  }
  // lp filter
  {
    p1p0 = _mm_unpacklo_epi64(q0p0, q1p1);
    q1q0 = _mm_unpackhi_epi64(q0p0, q1p1);

    highbd_filter4_sse2(&p1p0, &q1q0, &hev, &mask, &qs1qs0, &ps1ps0, &t80, bd);
  }

  qs1qs0 = _mm_andnot_si128(flat, qs1qs0);
  q1q0 = _mm_and_si128(flat, flat_q0q1);
  q1q0 = _mm_or_si128(qs1qs0, q1q0);

  ps1ps0 = _mm_andnot_si128(flat, ps1ps0);
  p1p0 = _mm_and_si128(flat, flat_p1p0);
  p1p0 = _mm_or_si128(ps1ps0, p1p0);

  _mm_storel_epi64((__m128i *)(s - 2 * p), _mm_srli_si128(p1p0, 8));
  _mm_storel_epi64((__m128i *)(s - 1 * p), p1p0);
  _mm_storel_epi64((__m128i *)(s + 0 * p), q1q0);
  _mm_storel_epi64((__m128i *)(s + 1 * p), _mm_srli_si128(q1q0, 8));
}

void aom_highbd_lpf_horizontal_8_sse2(uint16_t *s, int p,
                                      const uint8_t *_blimit,
                                      const uint8_t *_limit,
                                      const uint8_t *_thresh, int bd) {
  const __m128i zero = _mm_setzero_si128();
  __m128i blimit, limit, thresh;
  __m128i mask, hev, flat;
  __m128i p2, p1, p0, q0, q1, q2, p3, q3;
  __m128i q2p2, q1p1, q0p0, p1q1, p0q0, q3p3;
  __m128i p1p0, q1q0, ps1ps0, qs1qs0;
  __m128i work_a, op2, oq2, flat_p1p0, flat_q0q1;

  p3 = _mm_loadl_epi64((__m128i *)(s - 4 * p));
  q3 = _mm_loadl_epi64((__m128i *)(s + 3 * p));
  p2 = _mm_loadl_epi64((__m128i *)(s - 3 * p));
  q2 = _mm_loadl_epi64((__m128i *)(s + 2 * p));
  p1 = _mm_loadl_epi64((__m128i *)(s - 2 * p));
  q1 = _mm_loadl_epi64((__m128i *)(s + 1 * p));
  p0 = _mm_loadl_epi64((__m128i *)(s - 1 * p));
  q0 = _mm_loadl_epi64((__m128i *)(s + 0 * p));

  q3p3 = _mm_unpacklo_epi64(p3, q3);
  q2p2 = _mm_unpacklo_epi64(p2, q2);
  q1p1 = _mm_unpacklo_epi64(p1, q1);
  q0p0 = _mm_unpacklo_epi64(p0, q0);

  p1q1 = _mm_shuffle_epi32(q1p1, _MM_SHUFFLE(1, 0, 3, 2));
  p0q0 = _mm_shuffle_epi32(q0p0, _MM_SHUFFLE(1, 0, 3, 2));

  __m128i abs_p1q1, abs_p0q0, abs_p1p0, work;

  const __m128i four = _mm_set1_epi16(4);
  __m128i t80;
  const __m128i one = _mm_set1_epi16(0x1);
  const __m128i ffff = _mm_cmpeq_epi16(one, one);

  if (bd == 8) {
    blimit = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero);
    limit = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero);
    thresh = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero);
    t80 = _mm_set1_epi16(0x80);
  } else if (bd == 10) {
    blimit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero), 2);
    limit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero), 2);
    thresh = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero), 2);
    t80 = _mm_set1_epi16(0x200);
  } else {  // bd == 12
    blimit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero), 4);
    limit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero), 4);
    thresh = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero), 4);
    t80 = _mm_set1_epi16(0x800);
  }

  // filter_mask and hev_mask
  highbd_hev_mask(&p0q0, &p1q1, &thresh, &abs_p1p0, &hev);

  abs_p0q0 = abs_diff16(q0p0, p0q0);
  abs_p1q1 = abs_diff16(q1p1, p1q1);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(p0 - q0) * 2 + abs(p1 - q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit, one));
  mask = _mm_max_epi16(abs_p1p0, mask);
  // mask |= (abs(p1 - p0) > limit) * -1;
  // mask |= (abs(q1 - q0) > limit) * -1;

  work = _mm_max_epi16(abs_diff16(q2p2, q1p1), abs_diff16(q3p3, q2p2));
  mask = _mm_max_epi16(work, mask);
  mask = _mm_max_epi16(mask, _mm_srli_si128(mask, 8));
  mask = _mm_subs_epu16(mask, limit);
  mask = _mm_cmpeq_epi16(mask, zero);

  // flat_mask4
  flat = _mm_max_epi16(abs_diff16(q2p2, q0p0), abs_diff16(q3p3, q0p0));
  flat = _mm_max_epi16(abs_p1p0, flat);
  flat = _mm_max_epi16(flat, _mm_srli_si128(flat, 8));

  if (bd == 8)
    flat = _mm_subs_epu16(flat, one);
  else if (bd == 10)
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 2));
  else  // bd == 12
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 4));

  flat = _mm_cmpeq_epi16(flat, zero);
  flat = _mm_and_si128(flat, mask);  // flat & mask
  // replicate for the further "merged variables" usage
  flat = _mm_unpacklo_epi64(flat, flat);

  {
    __m128i workp_a, workp_b, workp_shft0, workp_shft1;
    // Added before shift for rounding part of ROUND_POWER_OF_TWO

    // op2
    workp_a = _mm_add_epi16(_mm_add_epi16(p3, p3), _mm_add_epi16(p2, p1));
    workp_a = _mm_add_epi16(_mm_add_epi16(workp_a, four), p0);
    workp_b = _mm_add_epi16(_mm_add_epi16(q0, p2), p3);
    op2 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // op1
    workp_b = _mm_add_epi16(_mm_add_epi16(q0, q1), p1);
    workp_shft0 = _mm_add_epi16(workp_a, workp_b);

    // op0
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, p3), q2);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, p1), p0);
    workp_shft1 = _mm_add_epi16(workp_a, workp_b);

    flat_p1p0 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_shft1, workp_shft0), 3);

    // oq0
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, p3), q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, p0), q0);
    workp_shft0 = _mm_add_epi16(workp_a, workp_b);

    // oq1
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, p2), q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, q0), q1);
    workp_shft1 = _mm_add_epi16(workp_a, workp_b);

    flat_q0q1 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_shft0, workp_shft1), 3);

    // oq2
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, p1), q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, q1), q2);
    oq2 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);
  }

  // lp filter
  {
    p1p0 = _mm_unpacklo_epi64(q0p0, q1p1);
    q1q0 = _mm_unpackhi_epi64(q0p0, q1p1);

    highbd_filter4_sse2(&p1p0, &q1q0, &hev, &mask, &qs1qs0, &ps1ps0, &t80, bd);
  }

  qs1qs0 = _mm_andnot_si128(flat, qs1qs0);
  q1q0 = _mm_and_si128(flat, flat_q0q1);
  q1q0 = _mm_or_si128(qs1qs0, q1q0);

  ps1ps0 = _mm_andnot_si128(flat, ps1ps0);
  p1p0 = _mm_and_si128(flat, flat_p1p0);
  p1p0 = _mm_or_si128(ps1ps0, p1p0);

  work_a = _mm_andnot_si128(flat, q2);
  q2 = _mm_and_si128(flat, oq2);
  q2 = _mm_or_si128(work_a, q2);

  work_a = _mm_andnot_si128(flat, p2);
  p2 = _mm_and_si128(flat, op2);
  p2 = _mm_or_si128(work_a, p2);

  _mm_storel_epi64((__m128i *)(s - 3 * p), p2);
  _mm_storel_epi64((__m128i *)(s - 2 * p), _mm_srli_si128(p1p0, 8));
  _mm_storel_epi64((__m128i *)(s - 1 * p), p1p0);
  _mm_storel_epi64((__m128i *)(s + 0 * p), q1q0);
  _mm_storel_epi64((__m128i *)(s + 1 * p), _mm_srli_si128(q1q0, 8));
  _mm_storel_epi64((__m128i *)(s + 2 * p), q2);
}

void aom_highbd_lpf_horizontal_8_dual_sse2(
    uint16_t *s, int p, const uint8_t *_blimit0, const uint8_t *_limit0,
    const uint8_t *_thresh0, const uint8_t *_blimit1, const uint8_t *_limit1,
    const uint8_t *_thresh1, int bd) {
  aom_highbd_lpf_horizontal_8_sse2(s, p, _blimit0, _limit0, _thresh0, bd);
  aom_highbd_lpf_horizontal_8_sse2(s + 4, p, _blimit1, _limit1, _thresh1, bd);
}

void aom_highbd_lpf_horizontal_4_sse2(uint16_t *s, int p,
                                      const uint8_t *_blimit,
                                      const uint8_t *_limit,
                                      const uint8_t *_thresh, int bd) {
  const __m128i zero = _mm_set1_epi16(0);
  __m128i blimit, limit, thresh;
  __m128i mask, hev, flat;
  __m128i p1 = _mm_loadu_si128((__m128i *)(s - 2 * p));
  __m128i p0 = _mm_loadu_si128((__m128i *)(s - 1 * p));
  __m128i q0 = _mm_loadu_si128((__m128i *)(s - 0 * p));
  __m128i q1 = _mm_loadu_si128((__m128i *)(s + 1 * p));
  __m128i abs_p0q0 = abs_diff16(q0, p0);
  __m128i abs_p1q1 = abs_diff16(q1, p1);

  __m128i abs_p1p0 = abs_diff16(p1, p0);
  __m128i abs_q1q0 = abs_diff16(q1, q0);

  const __m128i ffff = _mm_cmpeq_epi16(abs_p1p0, abs_p1p0);
  const __m128i one = _mm_set1_epi16(1);

  const __m128i t4 = _mm_set1_epi16(4);
  const __m128i t3 = _mm_set1_epi16(3);
  __m128i t80;
  __m128i tff80;
  __m128i tffe0;
  __m128i t1f;
  // equivalent to shifting 0x1f left by bitdepth - 8
  // and setting new bits to 1
  const __m128i t1 = _mm_set1_epi16(0x1);
  __m128i t7f;
  // equivalent to shifting 0x7f left by bitdepth - 8
  // and setting new bits to 1
  __m128i ps1, ps0, qs0, qs1;
  __m128i filt;
  __m128i work_a;
  __m128i filter1, filter2;

  if (bd == 8) {
    blimit = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero);
    limit = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero);
    thresh = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero);
    t80 = _mm_set1_epi16(0x80);
    tff80 = _mm_set1_epi16(0xff80);
    tffe0 = _mm_set1_epi16(0xffe0);
    t1f = _mm_srli_epi16(_mm_set1_epi16(0x1fff), 8);
    t7f = _mm_srli_epi16(_mm_set1_epi16(0x7fff), 8);
  } else if (bd == 10) {
    blimit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero), 2);
    limit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero), 2);
    thresh = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero), 2);
    t80 = _mm_slli_epi16(_mm_set1_epi16(0x80), 2);
    tff80 = _mm_slli_epi16(_mm_set1_epi16(0xff80), 2);
    tffe0 = _mm_slli_epi16(_mm_set1_epi16(0xffe0), 2);
    t1f = _mm_srli_epi16(_mm_set1_epi16(0x1fff), 6);
    t7f = _mm_srli_epi16(_mm_set1_epi16(0x7fff), 6);
  } else {  // bd == 12
    blimit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit), zero), 4);
    limit = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit), zero), 4);
    thresh = _mm_slli_epi16(
        _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh), zero), 4);
    t80 = _mm_slli_epi16(_mm_set1_epi16(0x80), 4);
    tff80 = _mm_slli_epi16(_mm_set1_epi16(0xff80), 4);
    tffe0 = _mm_slli_epi16(_mm_set1_epi16(0xffe0), 4);
    t1f = _mm_srli_epi16(_mm_set1_epi16(0x1fff), 4);
    t7f = _mm_srli_epi16(_mm_set1_epi16(0x7fff), 4);
  }

  ps1 = _mm_subs_epi16(_mm_loadu_si128((__m128i *)(s - 2 * p)), t80);
  ps0 = _mm_subs_epi16(_mm_loadu_si128((__m128i *)(s - 1 * p)), t80);
  qs0 = _mm_subs_epi16(_mm_loadu_si128((__m128i *)(s + 0 * p)), t80);
  qs1 = _mm_subs_epi16(_mm_loadu_si128((__m128i *)(s + 1 * p)), t80);

  // filter_mask and hev_mask
  flat = _mm_max_epi16(abs_p1p0, abs_q1q0);
  hev = _mm_subs_epu16(flat, thresh);
  hev = _mm_xor_si128(_mm_cmpeq_epi16(hev, zero), ffff);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(p0 - q0) * 2 + abs(p1 - q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit, one));
  mask = _mm_max_epi16(flat, mask);

  mask = _mm_subs_epu16(mask, limit);
  mask = _mm_cmpeq_epi16(mask, zero);

  // filter4
  const __m128i pmax =
      _mm_subs_epi16(_mm_subs_epi16(_mm_slli_epi16(one, bd), one), t80);
  const __m128i pmin = _mm_subs_epi16(zero, t80);

  filt = _mm_subs_epi16(ps1, qs1);
  pixel_clamp(&pmin, &pmax, &filt);
  filt = _mm_and_si128(filt, hev);
  work_a = _mm_subs_epi16(qs0, ps0);
  filt = _mm_adds_epi16(filt, work_a);
  filt = _mm_adds_epi16(filt, work_a);
  filt = _mm_adds_epi16(filt, work_a);
  pixel_clamp(&pmin, &pmax, &filt);

  // (aom_filter + 3 * (qs0 - ps0)) & mask
  filt = _mm_and_si128(filt, mask);

  filter1 = _mm_adds_epi16(filt, t4);
  pixel_clamp(&pmin, &pmax, &filter1);

  filter2 = _mm_adds_epi16(filt, t3);
  pixel_clamp(&pmin, &pmax, &filter2);

  // Filter1 >> 3
  work_a = _mm_cmpgt_epi16(zero, filter1);  // get the values that are <0
  filter1 = _mm_srli_epi16(filter1, 3);
  work_a = _mm_and_si128(work_a, tffe0);    // sign bits for the values < 0
  filter1 = _mm_and_si128(filter1, t1f);    // clamp the range
  filter1 = _mm_or_si128(filter1, work_a);  // reinsert the sign bits

  // Filter2 >> 3
  work_a = _mm_cmpgt_epi16(zero, filter2);
  filter2 = _mm_srli_epi16(filter2, 3);
  work_a = _mm_and_si128(work_a, tffe0);
  filter2 = _mm_and_si128(filter2, t1f);
  filter2 = _mm_or_si128(filter2, work_a);

  // filt >> 1
  filt = _mm_adds_epi16(filter1, t1);
  work_a = _mm_cmpgt_epi16(zero, filt);
  filt = _mm_srli_epi16(filt, 1);
  work_a = _mm_and_si128(work_a, tff80);
  filt = _mm_and_si128(filt, t7f);
  filt = _mm_or_si128(filt, work_a);

  filt = _mm_andnot_si128(hev, filt);

  q0 = _mm_subs_epi16(qs0, filter1);
  pixel_clamp(&pmin, &pmax, &q0);
  q0 = _mm_adds_epi16(q0, t80);

  q1 = _mm_subs_epi16(qs1, filt);
  pixel_clamp(&pmin, &pmax, &q1);
  q1 = _mm_adds_epi16(q1, t80);

  p0 = _mm_adds_epi16(ps0, filter2);
  pixel_clamp(&pmin, &pmax, &p0);
  p0 = _mm_adds_epi16(p0, t80);

  p1 = _mm_adds_epi16(ps1, filt);
  pixel_clamp(&pmin, &pmax, &p1);
  p1 = _mm_adds_epi16(p1, t80);
  _mm_storel_epi64((__m128i *)(s - 2 * p), p1);
  _mm_storel_epi64((__m128i *)(s - 1 * p), p0);
  _mm_storel_epi64((__m128i *)(s + 0 * p), q0);
  _mm_storel_epi64((__m128i *)(s + 1 * p), q1);
}

void aom_highbd_lpf_horizontal_4_dual_sse2(
    uint16_t *s, int p, const uint8_t *_blimit0, const uint8_t *_limit0,
    const uint8_t *_thresh0, const uint8_t *_blimit1, const uint8_t *_limit1,
    const uint8_t *_thresh1, int bd) {
  aom_highbd_lpf_horizontal_4_sse2(s, p, _blimit0, _limit0, _thresh0, bd);
  aom_highbd_lpf_horizontal_4_sse2(s + 4, p, _blimit1, _limit1, _thresh1, bd);
}

void aom_highbd_lpf_vertical_4_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                    const uint8_t *limit, const uint8_t *thresh,
                                    int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[8 * 8]);
  uint16_t *src[1];
  uint16_t *dst[1];

  // Transpose 8x8
  src[0] = s - 4;
  dst[0] = t_dst;

  highbd_transpose8x8(src, p, dst, 8, 1);

  // Loop filtering
  aom_highbd_lpf_horizontal_4_sse2(t_dst + 4 * 8, 8, blimit, limit, thresh, bd);

  src[0] = t_dst;
  dst[0] = s - 4;

  // Transpose back
  highbd_transpose8x8(src, 8, dst, p, 1);
}

void aom_highbd_lpf_vertical_4_dual_sse2(
    uint16_t *s, int p, const uint8_t *blimit0, const uint8_t *limit0,
    const uint8_t *thresh0, const uint8_t *blimit1, const uint8_t *limit1,
    const uint8_t *thresh1, int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[16 * 8]);
  uint16_t *src[2];
  uint16_t *dst[2];

  // Transpose 8x16
  highbd_transpose8x16(s - 4, s - 4 + p * 8, p, t_dst, 16);

  // Loop filtering
  aom_highbd_lpf_horizontal_4_dual_sse2(t_dst + 4 * 16, 16, blimit0, limit0,
                                        thresh0, blimit1, limit1, thresh1, bd);
  src[0] = t_dst;
  src[1] = t_dst + 8;
  dst[0] = s - 4;
  dst[1] = s - 4 + p * 8;

  // Transpose back
  highbd_transpose8x8(src, 16, dst, p, 2);
}

void aom_highbd_lpf_vertical_6_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                    const uint8_t *limit, const uint8_t *thresh,
                                    int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[38]);
  uint16_t *src[1];
  uint16_t *dst[1];

  // Transpose 6x6
  src[0] = s - 3;
  dst[0] = t_dst;

  highbd_transpose6x6(src, p, dst, 6, 1);

  // Loop filtering
  aom_highbd_lpf_horizontal_6_sse2(t_dst + 3 * 6, 6, blimit, limit, thresh, bd);

  src[0] = t_dst;
  dst[0] = s - 3;

  // Transpose back
  highbd_transpose6x6(src, 6, dst, p, 1);
}

void aom_highbd_lpf_vertical_8_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                    const uint8_t *limit, const uint8_t *thresh,
                                    int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[8 * 8]);
  uint16_t *src[1];
  uint16_t *dst[1];

  // Transpose 8x8
  src[0] = s - 4;
  dst[0] = t_dst;

  highbd_transpose8x8(src, p, dst, 8, 1);

  // Loop filtering
  aom_highbd_lpf_horizontal_8_sse2(t_dst + 4 * 8, 8, blimit, limit, thresh, bd);

  src[0] = t_dst;
  dst[0] = s - 4;

  // Transpose back
  highbd_transpose8x8(src, 8, dst, p, 1);
}

void aom_highbd_lpf_vertical_8_dual_sse2(
    uint16_t *s, int p, const uint8_t *blimit0, const uint8_t *limit0,
    const uint8_t *thresh0, const uint8_t *blimit1, const uint8_t *limit1,
    const uint8_t *thresh1, int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[16 * 8]);
  uint16_t *src[2];
  uint16_t *dst[2];

  // Transpose 8x16
  highbd_transpose8x16(s - 4, s - 4 + p * 8, p, t_dst, 16);

  // Loop filtering
  aom_highbd_lpf_horizontal_8_dual_sse2(t_dst + 4 * 16, 16, blimit0, limit0,
                                        thresh0, blimit1, limit1, thresh1, bd);
  src[0] = t_dst;
  src[1] = t_dst + 8;

  dst[0] = s - 4;
  dst[1] = s - 4 + p * 8;

  // Transpose back
  highbd_transpose8x8(src, 16, dst, p, 2);
}

void aom_highbd_lpf_vertical_14_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                     const uint8_t *limit,
                                     const uint8_t *thresh, int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[8 * 16]);
  uint16_t *src[2];
  uint16_t *dst[2];

  src[0] = s - 8;
  src[1] = s;
  dst[0] = t_dst;
  dst[1] = t_dst + 8 * 8;

  // Transpose 16x8
  highbd_transpose8x8(src, p, dst, 8, 2);

  // Loop filtering
  aom_highbd_lpf_horizontal_14_sse2(t_dst + 8 * 8, 8, blimit, limit, thresh,
                                    bd);
  src[0] = t_dst;
  src[1] = t_dst + 8 * 8;
  dst[0] = s - 8;
  dst[1] = s;

  // Transpose back
  highbd_transpose8x8(src, 8, dst, p, 2);
}

void aom_highbd_lpf_vertical_14_dual_sse2(uint16_t *s, int p,
                                          const uint8_t *blimit,
                                          const uint8_t *limit,
                                          const uint8_t *thresh, int bd) {
  DECLARE_ALIGNED(16, uint16_t, t_dst[256]);

  //  Transpose 16x16
  highbd_transpose8x16(s - 8, s - 8 + 8 * p, p, t_dst, 16);
  highbd_transpose8x16(s, s + 8 * p, p, t_dst + 8 * 16, 16);
  highbd_lpf_horz_edge_8_dual_sse2(t_dst + 8 * 16, 16, blimit, limit, thresh,
                                   bd);
  //  Transpose back
  highbd_transpose8x16(t_dst, t_dst + 8 * 16, 16, s - 8, p);
  highbd_transpose8x16(t_dst + 8, t_dst + 8 + 8 * 16, 16, s - 8 + 8 * p, p);
}
