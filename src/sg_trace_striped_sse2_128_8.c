/**
 * @file
 *
 * @author jeff.daily@pnnl.gov
 *
 * Copyright (c) 2015 Battelle Memorial Institute.
 */
#include "config.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <emmintrin.h>
#endif

#define SG_TRACE
#define SG_SUFFIX _striped_sse2_128_8
#define SG_SUFFIX_PROF _striped_profile_sse2_128_8
#include "sg_helper.h"

#include "parasail.h"
#include "parasail/memory.h"
#include "parasail/internal_sse.h"

#define SWAP(A,B) { __m128i* tmp = A; A = B; B = tmp; }

#define NEG_INF INT8_MIN

static inline __m128i _mm_blendv_epi8_rpl(__m128i a, __m128i b, __m128i mask) {
    a = _mm_andnot_si128(mask, a);
    a = _mm_or_si128(a, _mm_and_si128(mask, b));
    return a;
}

static inline __m128i _mm_insert_epi8_rpl(__m128i a, int8_t i, const int imm) {
    __m128i_8_t A;
    A.m = a;
    A.v[imm] = i;
    return A.m;
}

static inline __m128i _mm_max_epi8_rpl(__m128i a, __m128i b) {
    __m128i mask = _mm_cmpgt_epi8(a, b);
    a = _mm_and_si128(a, mask);
    b = _mm_andnot_si128(mask, b);
    return _mm_or_si128(a, b);
}

static inline int8_t _mm_extract_epi8_rpl(__m128i a, const int imm) {
    __m128i_8_t A;
    A.m = a;
    return A.v[imm];
}

static inline int8_t _mm_hmax_epi8_rpl(__m128i a) {
    a = _mm_max_epi8_rpl(a, _mm_srli_si128(a, 8));
    a = _mm_max_epi8_rpl(a, _mm_srli_si128(a, 4));
    a = _mm_max_epi8_rpl(a, _mm_srli_si128(a, 2));
    a = _mm_max_epi8_rpl(a, _mm_srli_si128(a, 1));
    return _mm_extract_epi8_rpl(a, 0);
}

static inline __m128i _mm_min_epi8_rpl(__m128i a, __m128i b) {
    __m128i mask = _mm_cmpgt_epi8(b, a);
    a = _mm_and_si128(a, mask);
    b = _mm_andnot_si128(mask, b);
    return _mm_or_si128(a, b);
}


static inline void arr_store(
        __m128i *array,
        __m128i vH,
        int32_t t,
        int32_t seglen,
        int32_t d)
{
    _mm_store_si128(array + (1LL*d*seglen+t), vH);
}

static inline __m128i arr_load(
        __m128i *array,
        int32_t t,
        int32_t seglen,
        int32_t d)
{
    return _mm_load_si128(array + (1LL*d*seglen+t));
}

#define FNAME parasail_sg_flags_trace_striped_sse2_128_8
#define PNAME parasail_sg_flags_trace_striped_profile_sse2_128_8

parasail_result_t* FNAME(
        const char * const restrict s1, const int s1Len,
        const char * const restrict s2, const int s2Len,
        const int open, const int gap, const parasail_matrix_t *matrix,
        int s1_beg, int s1_end, int s2_beg, int s2_end)
{
    parasail_profile_t *profile = parasail_profile_create_sse_128_8(s1, s1Len, matrix);
    parasail_result_t *result = PNAME(profile, s2, s2Len, open, gap, s1_beg, s1_end, s2_beg, s2_end);
    parasail_profile_free(profile);
    return result;
}

parasail_result_t* PNAME(
        const parasail_profile_t * const restrict profile,
        const char * const restrict s2, const int s2Len,
        const int open, const int gap,
        int s1_beg, int s1_end, int s2_beg, int s2_end)
{
    int32_t i = 0;
    int32_t j = 0;
    int32_t k = 0;
    int32_t end_query = 0;
    int32_t end_ref = 0;
    const int s1Len = profile->s1Len;
    const parasail_matrix_t *matrix = profile->matrix;
    const int32_t segWidth = 16; /* number of values in vector unit */
    const int32_t segLen = (s1Len + segWidth - 1) / segWidth;
    const int32_t offset = (s1Len - 1) % segLen;
    const int32_t position = (segWidth - 1) - (s1Len - 1) / segLen;
    __m128i* const restrict vProfile = (__m128i*)profile->profile8.score;
    __m128i* restrict pvHStore = parasail_memalign___m128i(16, segLen);
    __m128i* restrict pvHLoad = parasail_memalign___m128i(16, segLen);
    __m128i* const restrict pvE = parasail_memalign___m128i(16, segLen);
    __m128i* restrict pvEaStore = parasail_memalign___m128i(16, segLen);
    __m128i* restrict pvEaLoad = parasail_memalign___m128i(16, segLen);
    __m128i* const restrict pvHT = parasail_memalign___m128i(16, segLen);
    __m128i vGapO = _mm_set1_epi8(open);
    __m128i vGapE = _mm_set1_epi8(gap);
    __m128i vNegInf = _mm_set1_epi8(NEG_INF);
    int8_t score = NEG_INF;
    __m128i vMaxH = vNegInf;
    __m128i vPosMask = _mm_cmpeq_epi8(_mm_set1_epi8(position),
            _mm_set_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15));
    __m128i vNegLimit = _mm_set1_epi8(INT8_MIN);
    __m128i vPosLimit = _mm_set1_epi8(INT8_MAX);
    __m128i vSaturationCheckMin = vPosLimit;
    __m128i vSaturationCheckMax = vNegLimit;
    parasail_result_t *result = parasail_result_new_trace(segLen, s2Len, 16, sizeof(__m128i));
    __m128i vTIns  = _mm_set1_epi8(PARASAIL_INS);
    __m128i vTDel  = _mm_set1_epi8(PARASAIL_DEL);
    __m128i vTDiag = _mm_set1_epi8(PARASAIL_DIAG);
    __m128i vTDiagE = _mm_set1_epi8(PARASAIL_DIAG_E);
    __m128i vTInsE = _mm_set1_epi8(PARASAIL_INS_E);
    __m128i vTDiagF = _mm_set1_epi8(PARASAIL_DIAG_F);
    __m128i vTDelF = _mm_set1_epi8(PARASAIL_DEL_F);
    __m128i vTMask = _mm_set1_epi8(PARASAIL_ZERO_MASK);
    __m128i vFTMask = _mm_set1_epi8(PARASAIL_F_MASK);

    /* initialize H and E */
    parasail_memset___m128i(pvHStore, _mm_set1_epi8(0), segLen);
    parasail_memset___m128i(pvE, _mm_set1_epi8(-open), segLen);
    parasail_memset___m128i(pvEaStore, _mm_set1_epi8(-open), segLen);

    for (i=0; i<segLen; ++i) {
        arr_store(result->trace->trace_table, vTDiagE, i, segLen, 0);
    }

    /* outer loop over database sequence */
    for (j=0; j<s2Len; ++j) {
        __m128i vEF_opn;
        __m128i vE;
        __m128i vE_ext;
        __m128i vF;
        __m128i vF_ext;
        __m128i vFa;
        __m128i vFa_ext;
        __m128i vH;
        __m128i vH_dag;
        const __m128i* vP = NULL;

        /* Initialize F value to -inf.  Any errors to vH values will be
         * corrected in the Lazy_F loop. */
        vF = vNegInf;

        /* load final segment of pvHStore and shift left by 1 bytes */
        vH = _mm_load_si128(&pvHStore[segLen - 1]);
        vH = _mm_slli_si128(vH, 1);

        /* Correct part of the vProfile */
        vP = vProfile + matrix->mapper[(unsigned char)s2[j]] * segLen;

        /* Swap the 2 H buffers. */
        SWAP(pvHLoad, pvHStore)
        SWAP(pvEaLoad, pvEaStore)

        /* inner loop to process the query sequence */
        for (i=0; i<segLen; ++i) {
            vE = _mm_load_si128(pvE + i);

            /* Get max from vH, vE and vF. */
            vH_dag = _mm_adds_epi8(vH, _mm_load_si128(vP + i));
            vH = _mm_max_epi8_rpl(vH_dag, vE);
            vH = _mm_max_epi8_rpl(vH, vF);
            /* Save vH values. */
            _mm_store_si128(pvHStore + i, vH);
            /* check for saturation */
            {
                vSaturationCheckMax = _mm_max_epi8_rpl(vSaturationCheckMax, vH);
                vSaturationCheckMin = _mm_min_epi8_rpl(vSaturationCheckMin, vH);
                vSaturationCheckMin = _mm_min_epi8_rpl(vSaturationCheckMin, vE);
                vSaturationCheckMin = _mm_min_epi8_rpl(vSaturationCheckMin, vF);
            }

            {
                __m128i vTAll = arr_load(result->trace->trace_table, i, segLen, j);
                __m128i case1 = _mm_cmpeq_epi8(vH, vH_dag);
                __m128i case2 = _mm_cmpeq_epi8(vH, vF);
                __m128i vT = _mm_blendv_epi8_rpl(
                        _mm_blendv_epi8_rpl(vTIns, vTDel, case2),
                        vTDiag, case1);
                _mm_store_si128(pvHT + i, vT);
                vT = _mm_or_si128(vT, vTAll);
                arr_store(result->trace->trace_table, vT, i, segLen, j);
            }

            vEF_opn = _mm_subs_epi8(vH, vGapO);

            /* Update vE value. */
            vE_ext = _mm_subs_epi8(vE, vGapE);
            vE = _mm_max_epi8_rpl(vEF_opn, vE_ext);
            _mm_store_si128(pvE + i, vE);
            {
                __m128i vEa = _mm_load_si128(pvEaLoad + i);
                __m128i vEa_ext = _mm_subs_epi8(vEa, vGapE);
                vEa = _mm_max_epi8_rpl(vEF_opn, vEa_ext);
                _mm_store_si128(pvEaStore + i, vEa);
                if (j+1<s2Len) {
                    __m128i cond = _mm_cmpgt_epi8(vEF_opn, vEa_ext);
                    __m128i vT = _mm_blendv_epi8_rpl(vTInsE, vTDiagE, cond);
                    arr_store(result->trace->trace_table, vT, i, segLen, j+1);
                }
            }

            /* Update vF value. */
            vF_ext = _mm_subs_epi8(vF, vGapE);
            vF = _mm_max_epi8_rpl(vEF_opn, vF_ext);
            if (i+1<segLen) {
                __m128i vTAll = arr_load(result->trace->trace_table, i+1, segLen, j);
                __m128i cond = _mm_cmpgt_epi8(vEF_opn, vF_ext);
                __m128i vT = _mm_blendv_epi8_rpl(vTDelF, vTDiagF, cond);
                vT = _mm_or_si128(vT, vTAll);
                arr_store(result->trace->trace_table, vT, i+1, segLen, j);
            }

            /* Load the next vH. */
            vH = _mm_load_si128(pvHLoad + i);
        }

        /* Lazy_F loop: has been revised to disallow adjecent insertion and
         * then deletion, so don't update E(i, i), learn from SWPS3 */
        vFa_ext = vF_ext;
        vFa = vF;
        for (k=0; k<segWidth; ++k) {
            __m128i vHp = _mm_load_si128(&pvHLoad[segLen - 1]);
            vHp = _mm_slli_si128(vHp, 1);
            vEF_opn = _mm_slli_si128(vEF_opn, 1);
            vEF_opn = _mm_insert_epi8_rpl(vEF_opn, -open, 0);
            vF_ext = _mm_slli_si128(vF_ext, 1);
            vF_ext = _mm_insert_epi8_rpl(vF_ext, NEG_INF, 0);
            vF = _mm_slli_si128(vF, 1);
            vF = _mm_insert_epi8_rpl(vF, -open, 0);
            vFa_ext = _mm_slli_si128(vFa_ext, 1);
            vFa_ext = _mm_insert_epi8_rpl(vFa_ext, NEG_INF, 0);
            vFa = _mm_slli_si128(vFa, 1);
            vFa = _mm_insert_epi8_rpl(vFa, -open, 0);
            for (i=0; i<segLen; ++i) {
                vH = _mm_load_si128(pvHStore + i);
                vH = _mm_max_epi8_rpl(vH,vF);
                _mm_store_si128(pvHStore + i, vH);
                /* check for saturation */
            {
                vSaturationCheckMax = _mm_max_epi8_rpl(vSaturationCheckMax, vH);
                vSaturationCheckMin = _mm_min_epi8_rpl(vSaturationCheckMin, vH);
                vSaturationCheckMin = _mm_min_epi8_rpl(vSaturationCheckMin, vE);
                vSaturationCheckMin = _mm_min_epi8_rpl(vSaturationCheckMin, vF);
            }
                {
                    __m128i vTAll;
                    __m128i vT;
                    __m128i case1;
                    __m128i case2;
                    __m128i cond;
                    vHp = _mm_adds_epi8(vHp, _mm_load_si128(vP + i));
                    case1 = _mm_cmpeq_epi8(vH, vHp);
                    case2 = _mm_cmpeq_epi8(vH, vF);
                    cond = _mm_andnot_si128(case1,case2);
                    vTAll = arr_load(result->trace->trace_table, i, segLen, j);
                    vT = _mm_load_si128(pvHT + i);
                    vT = _mm_blendv_epi8_rpl(vT, vTDel, cond);
                    _mm_store_si128(pvHT + i, vT);
                    vTAll = _mm_and_si128(vTAll, vTMask);
                    vTAll = _mm_or_si128(vTAll, vT);
                    arr_store(result->trace->trace_table, vTAll, i, segLen, j);
                }
                /* Update vF value. */
                {
                    __m128i vTAll = arr_load(result->trace->trace_table, i, segLen, j);
                    __m128i cond = _mm_cmpgt_epi8(vEF_opn, vFa_ext);
                    __m128i vT = _mm_blendv_epi8_rpl(vTDelF, vTDiagF, cond);
                    vTAll = _mm_and_si128(vTAll, vFTMask);
                    vTAll = _mm_or_si128(vTAll, vT);
                    arr_store(result->trace->trace_table, vTAll, i, segLen, j);
                }
                vEF_opn = _mm_subs_epi8(vH, vGapO);
                vF_ext = _mm_subs_epi8(vF, vGapE);
                {
                    __m128i vEa = _mm_load_si128(pvEaLoad + i);
                    __m128i vEa_ext = _mm_subs_epi8(vEa, vGapE);
                    vEa = _mm_max_epi8_rpl(vEF_opn, vEa_ext);
                    _mm_store_si128(pvEaStore + i, vEa);
                    if (j+1<s2Len) {
                        __m128i cond = _mm_cmpgt_epi8(vEF_opn, vEa_ext);
                        __m128i vT = _mm_blendv_epi8_rpl(vTInsE, vTDiagE, cond);
                        arr_store(result->trace->trace_table, vT, i, segLen, j+1);
                    }
                }
                if (! _mm_movemask_epi8(
                            _mm_or_si128(
                                _mm_cmpgt_epi8(vF_ext, vEF_opn),
                                _mm_cmpeq_epi8(vF_ext, vEF_opn))))
                    goto end;
                /*vF = _mm_max_epi8_rpl(vEF_opn, vF_ext);*/
                vF = vF_ext;
                vFa_ext = _mm_subs_epi8(vFa, vGapE);
                vFa = _mm_max_epi8_rpl(vEF_opn, vFa_ext);
                vHp = _mm_load_si128(pvHLoad + i);
            }
        }
end:
        {
            /* extract vector containing last value from the column */
            __m128i vCompare;
            vH = _mm_load_si128(pvHStore + offset);
            vCompare = _mm_and_si128(vPosMask, _mm_cmpgt_epi8(vH, vMaxH));
            vMaxH = _mm_max_epi8_rpl(vH, vMaxH);
            if (_mm_movemask_epi8(vCompare)) {
                end_ref = j;
                end_query = s1Len - 1;
            }
        }
    }

    /* max last value from all columns */
    {
        for (k=0; k<position; ++k) {
            vMaxH = _mm_slli_si128(vMaxH, 1);
        }
        score = (int8_t) _mm_extract_epi8_rpl(vMaxH, 15);
    }

    /* max of last column */
    {
        int8_t score_last;
        vMaxH = vNegInf;

        for (i=0; i<segLen; ++i) {
            __m128i vH = _mm_load_si128(pvHStore + i);
            vMaxH = _mm_max_epi8_rpl(vH, vMaxH);
        }

        /* max in vec */
        score_last = _mm_hmax_epi8_rpl(vMaxH);
        if (score_last > score || (score_last == score && end_ref == s2Len - 1)) {
            score = score_last;
            end_ref = s2Len - 1;
            end_query = s1Len;
            /* Trace the alignment ending position on read. */
            {
                int8_t *t = (int8_t*)pvHStore;
                int32_t column_len = segLen * segWidth;
                for (i = 0; i<column_len; ++i, ++t) {
                    if (*t == score) {
                        int32_t temp = i / segWidth + i % segWidth * segLen;
                        if (temp < end_query) {
                            end_query = temp;
                        }
                    }
                }
            }
        }
    }

    if (_mm_movemask_epi8(_mm_or_si128(
            _mm_cmpeq_epi8(vSaturationCheckMin, vNegLimit),
            _mm_cmpeq_epi8(vSaturationCheckMax, vPosLimit)))) {
        result->flag |= PARASAIL_FLAG_SATURATED;
        score = INT8_MAX;
        end_query = 0;
        end_ref = 0;
    }

    result->score = score;
    result->end_query = end_query;
    result->end_ref = end_ref;
    result->flag |= PARASAIL_FLAG_SG | PARASAIL_FLAG_STRIPED
        | PARASAIL_FLAG_TRACE
        | PARASAIL_FLAG_BITS_8 | PARASAIL_FLAG_LANES_16;

    parasail_free(pvHT);
    parasail_free(pvEaLoad);
    parasail_free(pvEaStore);
    parasail_free(pvE);
    parasail_free(pvHLoad);
    parasail_free(pvHStore);

    return result;
}

SG_IMPL_ALL
SG_IMPL_PROF_ALL


