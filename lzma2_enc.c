/* lzma2_enc.c -- LZMA2 Encoder
Based on LzmaEnc.c and Lzma2Enc.c : Igor Pavlov
Modified for FL2 by Conor McCarthy
Public domain
*/

#include <stdlib.h>
#include <math.h>

#include "fl2_internal.h"
#include "lzma2_enc.h"
#include "fl2_compress_internal.h"
#include "mem.h"
#include "count.h"
#include "radix_mf.h"
#include "range_enc.h"

#ifdef FL2_XZ_BUILD
#  include "tuklib_integer.h"
#  define MEM_readLE32(a) unaligned_read32le(a)

#  ifdef TUKLIB_FAST_UNALIGNED_ACCESS
#    define MEM_read16(a) (*(const U16*)(a))
#  endif

#endif

#define kNumReps 4U
#define kNumStates 12U

#define kNumLiterals 0x100U
#define kNumLitTables 3U

#define kNumLenToPosStates 4U
#define kNumPosSlotBits 6U
#define kDicLogSizeMin 18U
#define kDicLogSizeMax 31U
#define kDistTableSizeMax (kDicLogSizeMax * 2U)

#define kNumAlignBits 4U
#define kAlignTableSize (1U << kNumAlignBits)
#define kAlignMask (kAlignTableSize - 1U)
#define kMatchRepriceFrequency 64U
#define kRepLenRepriceFrequency 64U

#define kStartPosModelIndex 4U
#define kEndPosModelIndex 14U
#define kNumPosModels (kEndPosModelIndex - kStartPosModelIndex)

#define kNumFullDistancesBits (kEndPosModelIndex >> 1U)
#define kNumFullDistances (1U << kNumFullDistancesBits)

#define kNumPositionBitsMax 4U
#define kNumPositionStatesMax (1U << kNumPositionBitsMax)
#define kNumLiteralContextBitsMax 4U
#define kNumLiteralPosBitsMax 4U
#define kLcLpMax 4U


#define kLenNumLowBits 3U
#define kLenNumLowSymbols (1U << kLenNumLowBits)
#define kLenNumHighBits 8U
#define kLenNumHighSymbols (1U << kLenNumHighBits)

#define kLenNumSymbolsTotal (kLenNumLowSymbols * 2 + kLenNumHighSymbols)

#define kMatchLenMin 2U
#define kMatchLenMax (kMatchLenMin + kLenNumSymbolsTotal - 1U)

#define kOptimizerBufferSize (1U << 11U)
#define kOptimizerEndSize 64U
#define kInfinityPrice (1UL << 30U)
#define kNullDist (U32)-1

#define kChunkSize ((1UL << 16U) - 8192U)
#define kSqrtChunkSize 239U
#define kTempMinOutput (LZMA_REQUIRED_INPUT_MAX * 4U)
#define kTempBufferSize (kTempMinOutput + kOptimizerBufferSize + kOptimizerBufferSize / 16U)
#define kMaxChunkUncompressedSize ((1UL << 21U) - kMatchLenMax)
#define kMaxChunkCompressedSize (1UL << 16U)
#define kChunkHeaderSize 5U
#define kChunkResetShift 5U
#define kChunkUncompressedDictReset 1U
#define kChunkUncompressed 2U
#define kChunkCompressedFlag 0x80U
#define kChunkNothingReset 0U
#define kChunkStateReset (1U << kChunkResetShift)
#define kChunkStatePropertiesReset (2U << kChunkResetShift)
#define kChunkAllReset (3U << kChunkResetShift)

#define kMaxHashDictBits 14U
#define kHash3Bits 14U
#define kNullLink -1

#define kMinTestChunkSize 0x4000U
#define kRandomFilterMarginBits 8U

#define kState_LitAfterMatch 4
#define kState_LitAfterRep   5
#define kState_MatchAfterLit 7
#define kState_RepAfterLit   8

static const BYTE kLiteralNextStates[kNumStates] = { 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5 };
#define LiteralNextState(s) kLiteralNextStates[s]
static const BYTE kMatchNextStates[kNumStates] = { 7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10 };
#define MatchNextState(s) kMatchNextStates[s]
static const BYTE kRepNextStates[kNumStates] = { 8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11 };
#define RepNextState(s) kRepNextStates[s]
static const BYTE kShortRepNextStates[kNumStates] = { 9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11 };
#define ShortRepNextState(s) kShortRepNextStates[s]

#include "fastpos_table.h"
#include "radix_get.h"

typedef struct 
{
    size_t table_size;
    unsigned prices[kNumPositionStatesMax][kLenNumSymbolsTotal];
    Probability choice; /* low[0] is choice_2. Must be consecutive for speed */
    Probability low[kNumPositionStatesMax << (kLenNumLowBits + 1)];
    Probability high[kLenNumHighSymbols];
} LengthStates;

typedef struct
{
    /* Fields are ordered for speed */
    LengthStates rep_len_states;
    Probability is_rep0_long[kNumStates][kNumPositionStatesMax];

    size_t state;
    U32 reps[kNumReps];

    Probability is_match[kNumStates][kNumPositionStatesMax];
    Probability is_rep[kNumStates];
    Probability is_rep_G0[kNumStates];
    Probability is_rep_G1[kNumStates];
    Probability is_rep_G2[kNumStates];

    LengthStates len_states;

    Probability dist_slot_encoders[kNumLenToPosStates][1 << kNumPosSlotBits];
    Probability dist_align_encoders[1 << kNumAlignBits];
    Probability dist_encoders[kNumFullDistances - kEndPosModelIndex];

    Probability literal_probs[(kNumLiterals * kNumLitTables) << kLcLpMax];
} EncoderStates;

typedef struct
{
    size_t state;
    U32 price;
    unsigned extra; /*  0   : normal
                     *  1   : LIT : MATCH
                     *  > 1 : MATCH (extra-1) : LIT : REP0 (len) */
    unsigned len;
    U32 dist;
    U32 reps[kNumReps];
} OptimalNode;

#define MakeAsLiteral(node) (node).dist = kNullDist; (node).extra = 0;
#define MakeAsShortRep(node) (node).dist = 0; (node).extra = 0;

typedef struct {
    S32 table_3[1 << kHash3Bits];
    S32 hash_chain_3[1];
} HashChains;

struct LZMA2_ECtx_s
{
    unsigned lc;
    unsigned lp;
    unsigned pb;
    unsigned fast_length;
    size_t len_end_max;
    size_t lit_pos_mask;
    size_t pos_mask;
    unsigned match_cycles;
    FL2_strategy strategy;

    RangeEncoder rc;

    EncoderStates states;

    unsigned match_price_count;
    unsigned rep_len_price_count;
    size_t dist_price_table_size;
    unsigned align_prices[kAlignTableSize];
    unsigned dist_slot_prices[kNumLenToPosStates][kDistTableSizeMax];
    unsigned distance_prices[kNumLenToPosStates][kNumFullDistances];

    RMF_match matches[kMatchLenMax-kMatchLenMin];
    size_t match_count;

    OptimalNode opt_buf[kOptimizerBufferSize];

    HashChains* hash_buf;
    ptrdiff_t chain_mask_2;
    ptrdiff_t chain_mask_3;
    ptrdiff_t hash_dict_3;
    ptrdiff_t hash_prev_index;
    ptrdiff_t hash_alloc_3;

    BYTE out_buf[kTempBufferSize];
};

LZMA2_ECtx* LZMA2_createECtx(void)
{
    LZMA2_ECtx *const enc = malloc(sizeof(LZMA2_ECtx));
    DEBUGLOG(3, "LZMA2_createECtx");
    if (enc == NULL)
        return NULL;

    enc->lc = 3;
    enc->lp = 0;
    enc->pb = 2;
    enc->fast_length = 48;
    enc->len_end_max = kOptimizerBufferSize - 1;
    enc->lit_pos_mask = (1 << enc->lp) - 1;
    enc->pos_mask = (1 << enc->pb) - 1;
    enc->match_cycles = 1;
    enc->strategy = FL2_ultra;
    enc->match_price_count = 0;
    enc->rep_len_price_count = 0;
    enc->dist_price_table_size = kDistTableSizeMax;
    enc->hash_buf = NULL;
    enc->hash_dict_3 = 0;
    enc->chain_mask_3 = 0;
    enc->hash_alloc_3 = 0;
    return enc;
}

void LZMA2_freeECtx(LZMA2_ECtx *const enc)
{
    if (enc == NULL)
        return;
    free(enc->hash_buf);
    free(enc);
}

#define GetLiteralProbs(enc, pos, prev_symbol) (enc->states.literal_probs + ((((pos) & enc->lit_pos_mask) << enc->lc) + ((prev_symbol) >> (8 - enc->lc))) * kNumLiterals * kNumLitTables)

#define GetLenToDistState(len) (((len) < kNumLenToPosStates + 1) ? (len) - 2 : kNumLenToPosStates - 1)

#define IsLitState(state) ((state) < 7)

HINT_INLINE
unsigned LZMA_getRepLen1Price(LZMA2_ECtx* const enc, size_t const state, size_t const pos_state)
{
    unsigned const rep_G0_prob = enc->states.is_rep_G0[state];
    unsigned const rep0_long_prob = enc->states.is_rep0_long[state][pos_state];
    return GET_PRICE_0(rep_G0_prob) + GET_PRICE_0(rep0_long_prob);
}

static unsigned LZMA_getRepPrice(LZMA2_ECtx* const enc, size_t const rep_index, size_t const state, size_t const pos_state)
{
    unsigned price;
    unsigned const rep_G0_prob = enc->states.is_rep_G0[state];
    if (rep_index == 0) {
        unsigned const rep0_long_prob = enc->states.is_rep0_long[state][pos_state];
        price = GET_PRICE_0(rep_G0_prob);
        price += GET_PRICE_1(rep0_long_prob);
    }
    else {
        unsigned const rep_G1_prob = enc->states.is_rep_G1[state];
        price = GET_PRICE_1(rep_G0_prob);
        if (rep_index == 1) {
            price += GET_PRICE_0(rep_G1_prob);
        }
        else {
            unsigned const rep_G2_prob = enc->states.is_rep_G2[state];
            price += GET_PRICE_1(rep_G1_prob);
            price += GET_PRICE(rep_G2_prob, (U32)(rep_index) - 2);
        }
    }
    return price;
}

static unsigned LZMA_getRepMatch0Price(LZMA2_ECtx *const enc, size_t const len, size_t const state, size_t const pos_state)
{
    unsigned const rep_G0_prob = enc->states.is_rep_G0[state];
    unsigned const rep0_long_prob = enc->states.is_rep0_long[state][pos_state];
    return enc->states.rep_len_states.prices[pos_state][len - kMatchLenMin]
        + GET_PRICE_0(rep_G0_prob)
        + GET_PRICE_1(rep0_long_prob);
}

static unsigned LZMA_getLiteralPriceMatched(const Probability *const prob_table, U32 symbol, unsigned match_byte)
{
    unsigned price = 0;
    unsigned offs = 0x100;
    symbol |= 0x100;
    do {
        match_byte <<= 1;
        price += GET_PRICE(prob_table[offs + (match_byte & offs) + (symbol >> 8)], (symbol >> 7) & 1);
        symbol <<= 1;
        offs &= ~(match_byte ^ symbol);
    } while (symbol < 0x10000);
    return price;
}

HINT_INLINE
void LZMA_encodeLiteral(LZMA2_ECtx *const enc, size_t const index, U32 symbol, unsigned const prev_symbol)
{
    RC_encodeBit0(&enc->rc, &enc->states.is_match[enc->states.state][index & enc->pos_mask]);
    enc->states.state = LiteralNextState(enc->states.state);

    Probability* const prob_table = GetLiteralProbs(enc, index, prev_symbol);
    symbol |= 0x100;
    do {
        RC_encodeBit(&enc->rc, prob_table + (symbol >> 8), symbol & (1 << 7));
        symbol <<= 1;
    } while (symbol < 0x10000);
}

HINT_INLINE
void LZMA_encodeLiteralMatched(LZMA2_ECtx *const enc, const BYTE* const data_block, size_t const index, U32 symbol)
{
    RC_encodeBit0(&enc->rc, &enc->states.is_match[enc->states.state][index & enc->pos_mask]);
    enc->states.state = LiteralNextState(enc->states.state);

    unsigned match_symbol = data_block[index - enc->states.reps[0] - 1];
    Probability* const prob_table = GetLiteralProbs(enc, index, data_block[index - 1]);
    unsigned offset = 0x100;
    symbol |= 0x100;
    do {
        match_symbol <<= 1;
        size_t prob_index = offset + (match_symbol & offset) + (symbol >> 8);
        RC_encodeBit(&enc->rc, prob_table + prob_index, symbol & (1 << 7));
        symbol <<= 1;
        offset &= ~(match_symbol ^ symbol);
    } while (symbol < 0x10000);
}

HINT_INLINE
void LZMA_encodeLiteralBuf(LZMA2_ECtx *const enc, const BYTE* const data_block, size_t const index)
{
    U32 const symbol = data_block[index];
    if (IsLitState(enc->states.state)) {
        unsigned const prev_symbol = data_block[index - 1];
        LZMA_encodeLiteral(enc, index, symbol, prev_symbol);
    }
    else {
        LZMA_encodeLiteralMatched(enc, data_block, index, symbol);
    }
}

static void LZMA_lengthStates_SetPrices(const Probability *probs, U32 start_price, U32 *prices)
{
    for (size_t i = 0; i < 8; i += 2) {
        U32 prob = probs[4 + (i >> 1)];
        U32 price = start_price + GET_PRICE(probs[1], (i >> 2))
            + GET_PRICE(probs[2 + (i >> 2)], (i >> 1) & 1);
        prices[i] = price + GET_PRICE_0(prob);
        prices[i + 1] = price + GET_PRICE_1(prob);
    }
}

static void FORCE_NOINLINE LZMA_lengthStates_updatePrices(LZMA2_ECtx *const enc, LengthStates* const ls)
{
    U32 b;

    {
        unsigned prob = ls->choice;
        U32 a, c;
        b = GET_PRICE_1(prob);
        a = GET_PRICE_0(prob);
        c = b + GET_PRICE_0(ls->low[0]);
        for (size_t pos_state = 0; pos_state <= enc->pos_mask; pos_state++)
        {
            U32 *prices = ls->prices[pos_state];
            const Probability *probs = ls->low + (pos_state << (1 + kLenNumLowBits));
            LZMA_lengthStates_SetPrices(probs, a, prices);
            LZMA_lengthStates_SetPrices(probs + kLenNumLowSymbols, c, prices + kLenNumLowSymbols);
        }
    }

    size_t i = ls->table_size;

    if (i > kLenNumLowSymbols * 2) {
        const Probability *probs = ls->high;
        U32 *prices = ls->prices[0] + kLenNumLowSymbols * 2;
        i = (i - (kLenNumLowSymbols * 2 - 1)) >> 1;
        b += GET_PRICE_1(ls->low[0]);
        do {
            --i;
            size_t sym = i + (1 << (kLenNumHighBits - 1));
            U32 price = b;
            do {
                unsigned bit = (unsigned)sym & 1;
                sym >>= 1;
                price += GET_PRICE(probs[sym], bit);
            } while (sym >= 2);

            unsigned prob = probs[i + (1 << (kLenNumHighBits - 1))];
            prices[i * 2] = price + GET_PRICE_0(prob);
            prices[i * 2 + 1] = price + GET_PRICE_1(prob);
        } while (i);

        size_t size = (ls->table_size - kLenNumLowSymbols * 2) * sizeof(ls->prices[0][0]);
        for (size_t pos_state = 1; pos_state <= enc->pos_mask; pos_state++)
            memcpy(ls->prices[pos_state] + kLenNumLowSymbols * 2, ls->prices[0] + kLenNumLowSymbols * 2, size);
    }
}

FORCE_NOINLINE
static void LZMA_encodeLength_MidHigh(LZMA2_ECtx *const enc, LengthStates* const len_prob_table, unsigned len, size_t const pos_state)
{
        RC_encodeBit1(&enc->rc, &len_prob_table->choice);
        if (len < kLenNumLowSymbols * 2) {
            RC_encodeBit0(&enc->rc, &len_prob_table->low[0]);
            RC_encodeBitTree(&enc->rc, len_prob_table->low + kLenNumLowSymbols + (pos_state << (1 + kLenNumLowBits)), kLenNumLowBits, len - kLenNumLowSymbols);
        }
        else {
            RC_encodeBit1(&enc->rc, &len_prob_table->low[0]);
            RC_encodeBitTree(&enc->rc, len_prob_table->high, kLenNumHighBits, len - kLenNumLowSymbols * 2);
        }
}

HINT_INLINE
void LZMA_encodeLength(LZMA2_ECtx *const enc, LengthStates* const len_prob_table, unsigned len, size_t const pos_state)
{
    len -= kMatchLenMin;
    if (len < kLenNumLowSymbols) {
        RC_encodeBit0(&enc->rc, &len_prob_table->choice);
        RC_encodeBitTree(&enc->rc, len_prob_table->low + (pos_state << (1 + kLenNumLowBits)), kLenNumLowBits, len);
    }
    else {
        LZMA_encodeLength_MidHigh(enc, len_prob_table, len, pos_state);
    }
}

FORCE_NOINLINE
static void LZMA_encodeRepMatch(LZMA2_ECtx *const enc, unsigned const len, unsigned const rep, size_t const pos_state)
{
    DEBUGLOG(7, "LZMA_encodeRepMatch : length %u, rep %u", len, rep);
    RC_encodeBit1(&enc->rc, &enc->states.is_match[enc->states.state][pos_state]);
    RC_encodeBit1(&enc->rc, &enc->states.is_rep[enc->states.state]);
    if (rep == 0) {
        RC_encodeBit0(&enc->rc, &enc->states.is_rep_G0[enc->states.state]);
        RC_encodeBit(&enc->rc, &enc->states.is_rep0_long[enc->states.state][pos_state], ((len == 1) ? 0 : 1));
    }
    else {
        U32 const distance = enc->states.reps[rep];
        RC_encodeBit1(&enc->rc, &enc->states.is_rep_G0[enc->states.state]);
        if (rep == 1) {
            RC_encodeBit0(&enc->rc, &enc->states.is_rep_G1[enc->states.state]);
        }
        else {
            RC_encodeBit1(&enc->rc, &enc->states.is_rep_G1[enc->states.state]);
            RC_encodeBit(&enc->rc, &enc->states.is_rep_G2[enc->states.state], rep - 2);
            if (rep == 3) {
                enc->states.reps[3] = enc->states.reps[2];
            }
            enc->states.reps[2] = enc->states.reps[1];
        }
        enc->states.reps[1] = enc->states.reps[0];
        enc->states.reps[0] = distance;
    }
    if (len != 1) {
        LZMA_encodeLength(enc, &enc->states.rep_len_states, len, pos_state);
        enc->states.state = RepNextState(enc->states.state);
        ++enc->rep_len_price_count;
    }
    else {
        enc->states.state = ShortRepNextState(enc->states.state);
    }
}

/* *****************************************/
/* Distance slot functions based on fastpos.h from XZ*/

HINT_INLINE
unsigned LZMA_fastDistShift(unsigned const n)
{
    return n * (kFastDistBits - 1);
}

HINT_INLINE
unsigned LZMA_fastDistResult(U32 const dist, unsigned const n)
{
    return distance_table[dist >> LZMA_fastDistShift(n)]
        + 2 * LZMA_fastDistShift(n);
}

static size_t LZMA_getDistSlot(U32 const distance)
{
    U32 limit = 1UL << kFastDistBits;
    /* If it is small enough, we can pick the result directly from */
    /* the precalculated table. */
    if (distance < limit) {
        return distance_table[distance];
    }
    limit <<= LZMA_fastDistShift(1);
    if (distance < limit) {
        return LZMA_fastDistResult(distance, 1);
    }
    return LZMA_fastDistResult(distance, 2);
}

/* **************************************** */

HINT_INLINE
void LZMA_encodeNormalMatch(LZMA2_ECtx *const enc, unsigned const len, U32 const dist, size_t const pos_state)
{
    DEBUGLOG(7, "LZMA_encodeNormalMatch : length %u, dist %u", len, dist);
    RC_encodeBit1(&enc->rc, &enc->states.is_match[enc->states.state][pos_state]);
    RC_encodeBit0(&enc->rc, &enc->states.is_rep[enc->states.state]);
    enc->states.state = MatchNextState(enc->states.state);
    LZMA_encodeLength(enc, &enc->states.len_states, len, pos_state);

    size_t const dist_slot = LZMA_getDistSlot(dist);
    RC_encodeBitTree(&enc->rc, enc->states.dist_slot_encoders[GetLenToDistState(len)], kNumPosSlotBits, (unsigned)(dist_slot));
    if (dist_slot >= kStartPosModelIndex) {
        unsigned const footer_bits = ((unsigned)(dist_slot >> 1) - 1);
        size_t const base = ((2 | (dist_slot & 1)) << footer_bits);
        unsigned dist_reduced = (unsigned)(dist - base);
        if (dist_slot < kEndPosModelIndex) {
            RC_encodeBitTreeReverse(&enc->rc, enc->states.dist_encoders + base - dist_slot - 1, footer_bits, dist_reduced);
        }
        else {
            RC_encodeDirect(&enc->rc, dist_reduced >> kNumAlignBits, footer_bits - kNumAlignBits);
            RC_encodeBitTreeReverse(&enc->rc, enc->states.dist_align_encoders, kNumAlignBits, dist_reduced & kAlignMask);
        }
    }
    enc->states.reps[3] = enc->states.reps[2];
    enc->states.reps[2] = enc->states.reps[1];
    enc->states.reps[1] = enc->states.reps[0];
    enc->states.reps[0] = dist;
    ++enc->match_price_count;
}

#if defined(_MSC_VER)
#  pragma warning(disable : 4701)  /* disable: C4701: potentially uninitialized local variable */
#endif

FORCE_INLINE_TEMPLATE
size_t LZMA_encodeChunkFast(LZMA2_ECtx *const enc,
    FL2_dataBlock const block,
    FL2_matchTable* const tbl,
    int const struct_tbl,
    size_t index,
    size_t const uncompressed_end)
{
    size_t const pos_mask = enc->pos_mask;
    size_t prev = index;
    unsigned const search_depth = tbl->params.depth;
    while (index < uncompressed_end && enc->rc.out_index < enc->rc.chunk_size)
    {
        size_t max_len;
        const BYTE* data;
        /* Table of distance restrictions for short matches */
        static const U32 max_dist_table[] = { 0, 0, 0, 1 << 6, 1 << 14 };
        /* Get a match from the table, extended to its full length */
        RMF_match best_match = RMF_getMatch(block, tbl, search_depth, struct_tbl, index);
        if (best_match.length < kMatchLenMin) {
            ++index;
            continue;
        }
        /* Use if near enough */
        if (best_match.length >= 5 || best_match.dist < max_dist_table[best_match.length]) {
            best_match.dist += kNumReps;
        }
        else {
            best_match.length = 0;
        }
        max_len = MIN(kMatchLenMax, block.end - index);
        data = block.data + index;

        RMF_match best_rep;
        RMF_match rep_match;
        best_rep.length = 0;
        for (rep_match.dist = 0; rep_match.dist < kNumReps; ++rep_match.dist) {
            const BYTE *data_2 = data - enc->states.reps[rep_match.dist] - 1;
            if (MEM_read16(data) != MEM_read16(data_2)) {
                continue;
            }
            rep_match.length = (U32)(ZSTD_count(data + 2, data_2 + 2, data + max_len) + 2);
            if (rep_match.length >= max_len) {
                best_match = rep_match;
                goto _encode;
            }
            if (rep_match.length > best_rep.length) {
                best_rep = rep_match;
            }
        }
        if (best_match.length >= max_len)
            goto _encode;
        if (best_rep.length >= 2) {
            int const gain2 = (int)(best_rep.length * 3 - best_rep.dist);
            int const gain1 = (int)(best_match.length * 3 - ZSTD_highbit32(best_match.dist + 1) + 1);
            if (gain2 > gain1) {
                DEBUGLOG(7, "Replace match (%u, %u) with rep (%u, %u)", best_match.length, best_match.dist, best_rep.length, best_rep.dist);
                best_match = best_rep;
            }
        }

        if (best_match.length < kMatchLenMin) {
            ++index;
            continue;
        }

        for (size_t next = index + 1; best_match.length < kMatchLenMax && next < uncompressed_end; ++next) {
            /* lazy matching scheme from ZSTD */
            RMF_match next_match = RMF_getNextMatch(block, tbl, search_depth, struct_tbl, next);
            if (next_match.length >= kMatchLenMin) {
                best_rep.length = 0;
                data = block.data + next;
                max_len = MIN(kMatchLenMax, block.end - next);
                for (rep_match.dist = 0; rep_match.dist < kNumReps; ++rep_match.dist) {
                    const BYTE *data_2 = data - enc->states.reps[rep_match.dist] - 1;
                    if (MEM_read16(data) != MEM_read16(data_2)) {
                        continue;
                    }
                    rep_match.length = (U32)(ZSTD_count(data + 2, data_2 + 2, data + max_len) + 2);
                    if (rep_match.length > best_rep.length) {
                        best_rep = rep_match;
                    }
                }
                if (best_rep.length >= 3) {
                    int const gain2 = (int)(best_rep.length * 3 - best_rep.dist);
                    int const gain1 = (int)(best_match.length * 3 - ZSTD_highbit32((U32)best_match.dist + 1) + 1);
                    if (gain2 > gain1) {
                        DEBUGLOG(7, "Replace match (%u, %u) with rep (%u, %u)", best_match.length, best_match.dist, best_rep.length, best_rep.dist);
                        best_match = best_rep;
                        index = next;
                    }
                }
                if (next_match.length >= 3 && next_match.dist != best_match.dist) {
                    int const gain2 = (int)(next_match.length * 4 - ZSTD_highbit32((U32)next_match.dist + 1));   /* raw approx */
                    int const gain1 = (int)(best_match.length * 4 - ZSTD_highbit32((U32)best_match.dist + 1) + 4);
                    if (gain2 > gain1) {
                        DEBUGLOG(7, "Replace match (%u, %u) with match (%u, %u)", best_match.length, best_match.dist, next_match.length, next_match.dist + kNumReps);
                        best_match = next_match;
                        best_match.dist += kNumReps;
                        index = next;
                        continue;
                    }
                }
            }
            if (next < uncompressed_end - 4) {
                ++next;
                next_match = RMF_getNextMatch(block, tbl, search_depth, struct_tbl, next);
                if (next_match.length < 4)
                    break;
                data = block.data + next;
                max_len = MIN(kMatchLenMax, block.end - next);
                best_rep.length = 0;
                for (rep_match.dist = 0; rep_match.dist < kNumReps; ++rep_match.dist) {
                    const BYTE *data_2 = data - enc->states.reps[rep_match.dist] - 1;
                    if (MEM_read16(data) != MEM_read16(data_2)) {
                        continue;
                    }
                    rep_match.length = (U32)(ZSTD_count(data + 2, data_2 + 2, data + max_len) + 2);
                    if (rep_match.length > best_rep.length) {
                        best_rep = rep_match;
                    }
                }
                if (best_rep.length >= 4) {
                    int const gain2 = (int)(best_rep.length * 4 - (best_rep.dist >> 1));
                    int const gain1 = (int)(best_match.length * 4 - ZSTD_highbit32((U32)best_match.dist + 1) + 1);
                    if (gain2 > gain1) {
                        DEBUGLOG(7, "Replace match (%u, %u) with rep (%u, %u)", best_match.length, best_match.dist, best_rep.length, best_rep.dist);
                        best_match = best_rep;
                        index = next;
                    }
                }
                if (next_match.length >= 4 && next_match.dist != best_match.dist) {
                    int const gain2 = (int)(next_match.length * 4 - ZSTD_highbit32((U32)next_match.dist + 1));
                    int const gain1 = (int)(best_match.length * 4 - ZSTD_highbit32((U32)best_match.dist + 1) + 7);
                    if (gain2 > gain1) {
                        DEBUGLOG(7, "Replace match (%u, %u) with match (%u, %u)", best_match.length, best_match.dist, next_match.length, next_match.dist + kNumReps);
                        best_match = next_match;
                        best_match.dist += kNumReps;
                        index = next;
                        continue;
                    }
                }

            }
            break;
        }
_encode:
        assert(index + best_match.length <= block.end);
        while (prev < index && enc->rc.out_index < enc->rc.chunk_size) {
            if (block.data[prev] == block.data[prev - enc->states.reps[0] - 1]) {
                LZMA_encodeRepMatch(enc, 1, 0, prev & pos_mask);
            }
            else {
                LZMA_encodeLiteralBuf(enc, block.data, prev);
            }
            ++prev;
        }
        if (enc->rc.out_index >= enc->rc.chunk_size) {
            break;
        }
        if(best_match.length >= kMatchLenMin) {
            if (best_match.dist < kNumReps) {
                LZMA_encodeRepMatch(enc, best_match.length, best_match.dist, index & pos_mask);
            }
            else {
                LZMA_encodeNormalMatch(enc, best_match.length, best_match.dist - kNumReps, index & pos_mask);
            }
            index += best_match.length;
            prev = index;
        }
    }
    while (prev < index && enc->rc.out_index < enc->rc.chunk_size) {
        if (block.data[prev] == block.data[prev - enc->states.reps[0] - 1]) {
            LZMA_encodeRepMatch(enc, 1, 0, prev & pos_mask);
        }
        else {
            LZMA_encodeLiteralBuf(enc, block.data, prev);
        }
        ++prev;
    }
    return prev;
}

/* Reverse the direction of the linked list generated by the optimal parser */
FORCE_NOINLINE
static void LZMA_reverseOptimalChain(OptimalNode* const opt_buf, size_t cur)
{
    unsigned len = (unsigned)opt_buf[cur].len;
    U32 dist = opt_buf[cur].dist;

    for(;;) {
        unsigned extra = (unsigned)opt_buf[cur].extra;
        cur -= len;

        if (extra) {
            opt_buf[cur].len = (U32)len;
            len = extra;
            if (extra == 1) {
                opt_buf[cur].dist = dist;
                dist = kNullDist;
                --cur;
            }
            else {
                opt_buf[cur].dist = 0;
                --cur;
                --len;
                opt_buf[cur].dist = kNullDist;
                opt_buf[cur].len = 1;
                cur -= len;
            }
        }

        unsigned next_len = opt_buf[cur].len;
        U32 next_dist = opt_buf[cur].dist;

        opt_buf[cur].dist = dist;
        opt_buf[cur].len = (U32)len;

        if (cur == 0)
            break;

        len = next_len;
        dist = next_dist;
    }
}

static unsigned LZMA_getLiteralPrice(LZMA2_ECtx *const enc, size_t const index, size_t const state, unsigned const prev_symbol, U32 symbol, unsigned const match_byte)
{
    const Probability* const prob_table = GetLiteralProbs(enc, index, prev_symbol);
    if (IsLitState(state)) {
        unsigned price = 0;
        symbol |= 0x100;
        do {
            price += GET_PRICE(prob_table[symbol >> 8], (symbol >> 7) & 1);
            symbol <<= 1;
        } while (symbol < 0x10000);
        return price;
    }
    return LZMA_getLiteralPriceMatched(prob_table, symbol, match_byte);
}

static void LZMA_hashReset(LZMA2_ECtx *const enc, unsigned const dictionary_bits_3)
{
    enc->hash_dict_3 = (ptrdiff_t)1 << dictionary_bits_3;
    enc->chain_mask_3 = enc->hash_dict_3 - 1;
    memset(enc->hash_buf->table_3, 0xFF, sizeof(enc->hash_buf->table_3));
}

static int LZMA_hashCreate(LZMA2_ECtx *const enc, unsigned const dictionary_bits_3)
{
    DEBUGLOG(3, "Create hash chain : dict bits %u", dictionary_bits_3);

    if (enc->hash_buf)
        free(enc->hash_buf);

    enc->hash_alloc_3 = (ptrdiff_t)1 << dictionary_bits_3;
    enc->hash_buf = malloc(sizeof(HashChains) + (enc->hash_alloc_3 - 1) * sizeof(S32));

    if (enc->hash_buf == NULL)
        return 1;

    LZMA_hashReset(enc, dictionary_bits_3);

    return 0;
}

/* Create a hash chain for hybrid mode */
int LZMA2_hashAlloc(LZMA2_ECtx *const enc, const FL2_lzma2Parameters* const options)
{
    if (enc->strategy == FL2_ultra && enc->hash_alloc_3 < ((ptrdiff_t)1 << options->second_dict_bits))
        return LZMA_hashCreate(enc, options->second_dict_bits);

    return 0;
}

#define GET_HASH_3(data) ((((MEM_readLE32(data)) << 8) * 506832829U) >> (32 - kHash3Bits))

HINT_INLINE
size_t LZMA_hashGetMatches(LZMA2_ECtx *const enc, FL2_dataBlock const block,
    ptrdiff_t const index,
    size_t const length_limit,
    RMF_match const match)
{
    ptrdiff_t const hash_dict_3 = enc->hash_dict_3;
    const BYTE* data = block.data;
    HashChains* const tbl = enc->hash_buf;
    ptrdiff_t const chain_mask_3 = enc->chain_mask_3;

    enc->match_count = 0;
    enc->hash_prev_index = MAX(enc->hash_prev_index, index - hash_dict_3);
    /* Update hash tables and chains for any positions that were skipped */
    while (++enc->hash_prev_index < index) {
        size_t hash = GET_HASH_3(data + enc->hash_prev_index);
        tbl->hash_chain_3[enc->hash_prev_index & chain_mask_3] = tbl->table_3[hash];
        tbl->table_3[hash] = (S32)enc->hash_prev_index;
    }
    data += index;

    size_t const hash = GET_HASH_3(data);
    ptrdiff_t const first_3 = tbl->table_3[hash];
    tbl->table_3[hash] = (S32)(index);

    size_t max_len = 2;

    if (first_3 >= 0) {
        int cycles = enc->match_cycles;
        ptrdiff_t const end_index = index - (((ptrdiff_t)match.dist < hash_dict_3) ? match.dist : hash_dict_3);
        ptrdiff_t match_3 = first_3;
        if (match_3 >= end_index) {
            do {
                --cycles;
                const BYTE* data_2 = block.data + match_3;
                size_t len_test = ZSTD_count(data + 1, data_2 + 1, data + length_limit) + 1;
                if (len_test > max_len) {
                    enc->matches[enc->match_count].length = (U32)len_test;
                    enc->matches[enc->match_count].dist = (U32)(index - match_3 - 1);
                    ++enc->match_count;
                    max_len = len_test;
                    if (len_test >= length_limit) {
                        break;
                    }
                }
                if (cycles <= 0)
                    break;
                match_3 = tbl->hash_chain_3[match_3 & chain_mask_3];
            } while (match_3 >= end_index);
        }
    }
    tbl->hash_chain_3[index & chain_mask_3] = (S32)first_3;
    if ((unsigned)max_len < match.length) {
        enc->matches[enc->match_count] = match;
        ++enc->match_count;
        return match.length;
    }
    return max_len;
}

#if defined(_MSC_VER)
#  pragma warning(disable : 4701)  /* disable: C4701: potentially uninitialized local variable */
#endif

/* The speed of this function is critical and the sections have so many variables
* in common that breaking it up would be inefficient.
* For each position cur, starting at 1, check some or all possible
* encoding choices - a literal, 1-byte rep 0 match, all rep match lengths, and
* all match lengths at available distances. It also checks the combined
* sequences literal+rep0, rep+rep0 and match+rep0.
* If is_hybrid != 0, this method works in hybrid mode, using the
* hash chain to find shorter matches at near distances. */
FORCE_INLINE_TEMPLATE
size_t LZMA_optimalParse(LZMA2_ECtx* const enc, FL2_dataBlock const block,
    RMF_match match,
    size_t const index,
    size_t const cur,
    size_t len_end,
    int const is_hybrid,
    U32* const reps)
{
    OptimalNode* const cur_opt = &enc->opt_buf[cur];
    size_t const pos_mask = enc->pos_mask;
    size_t const pos_state = (index & pos_mask);
    const BYTE* const data = block.data + index;
    size_t const fast_length = enc->fast_length;
    size_t prev_index = cur - cur_opt->len;
    size_t state;
    size_t bytes_avail;
    U32 match_price;
    U32 rep_match_price;

    if (cur_opt->len == 1) {
        const BYTE *next_state = (cur_opt->dist == 0) ? kShortRepNextStates : kLiteralNextStates;
        state = next_state[enc->opt_buf[prev_index].state];
    }
    else {
        size_t dist = cur_opt->dist;

        if (cur_opt->extra) {
            prev_index -= cur_opt->extra;
            state = kState_RepAfterLit - ((dist >= kNumReps) & (cur_opt->extra == 1));
        }
        else {
            state = enc->opt_buf[prev_index].state;
            state = MatchNextState(state) + (dist < kNumReps);
        }
        const OptimalNode* prev_opt = &enc->opt_buf[prev_index];
        if (dist < kNumReps) {
            reps[0] = prev_opt->reps[dist];
            size_t table = 1 | (2 << 2) | (3 << 4)      /* It probably doesn't save much time, */
                | (0 << 8) | (2 << 10) | (3 << 12)      /* but I like it because it's hideous :D */
                | (0L << 16) | (1L << 18) | (3L << 20)
                | (0L << 24) | (1L << 26) | (2L << 28);
            table >>= (dist << 3);
            reps[1] = prev_opt->reps[table & 3];
            table >>= 2;
            reps[2] = prev_opt->reps[table & 3];
            table >>= 2;
            reps[3] = prev_opt->reps[table & 3];
        }
        else {
            reps[0] = (U32)(dist - kNumReps);
            reps[1] = prev_opt->reps[0];
            reps[2] = prev_opt->reps[1];
            reps[3] = prev_opt->reps[2];
        }
    }
    cur_opt->state = state;
    memcpy(cur_opt->reps, reps, sizeof(cur_opt->reps));
    Probability is_rep_prob = enc->states.is_rep[state];

    {   Probability is_match_prob = enc->states.is_match[state][pos_state];
        unsigned cur_byte = *data;
        unsigned match_byte = *(data - reps[0] - 1);
        U32 cur_price = cur_opt->price;
        OptimalNode* next_opt = &enc->opt_buf[cur + 1];
        BYTE next_is_lit = 0;
        U32 cur_and_lit_price = cur_price + GET_PRICE_0(is_match_prob);
        if (cur_and_lit_price + kMinLitPrice / 2U > next_opt->price) {
            cur_and_lit_price = 0;
        }
        else {
            cur_and_lit_price += LZMA_getLiteralPrice(enc, index, state, data[-1], cur_byte, match_byte);
            /* Try literal */
            if (cur_and_lit_price < next_opt->price) {
                next_opt->price = cur_and_lit_price;
                next_opt->len = 1;
                MakeAsLiteral(*next_opt);
                next_is_lit = 1;
            }
        }
        match_price = cur_price + GET_PRICE_1(is_match_prob);
        rep_match_price = match_price + GET_PRICE_1(is_rep_prob);
        if (match_byte == cur_byte) {
            /* Try 1-byte rep0 */
            U32 short_rep_price = rep_match_price + LZMA_getRepLen1Price(enc, state, pos_state);
            if (short_rep_price <= next_opt->price) {
                next_opt->price = short_rep_price;
                next_opt->len = 1;
                MakeAsShortRep(*next_opt);
                next_is_lit = 1;
            }
        }
        bytes_avail = MIN(block.end - index, kOptimizerBufferSize - 1 - cur);
        if (bytes_avail < 2)
            return len_end;
        if (!next_is_lit && match_byte != cur_byte && cur_and_lit_price != 0) {
            /* Try literal + rep0 */
            const BYTE *data_2 = data - reps[0];
            size_t limit = MIN(bytes_avail - 1, fast_length);
            size_t len_test_2 = ZSTD_count(data + 1, data_2, data + 1 + limit);
            if (len_test_2 >= 2) {
                size_t state_2 = LiteralNextState(state);
                size_t pos_state_next = (index + 1) & pos_mask;
                U32 next_rep_match_price = cur_and_lit_price +
                    GET_PRICE_1(enc->states.is_match[state_2][pos_state_next]) +
                    GET_PRICE_1(enc->states.is_rep[state_2]);
                U32 cur_and_len_price = next_rep_match_price + LZMA_getRepMatch0Price(enc, len_test_2, state_2, pos_state_next);
                size_t offset = cur + 1 + len_test_2;
                if (cur_and_len_price < enc->opt_buf[offset].price) {
                    len_end = MAX(len_end, offset);
                    enc->opt_buf[offset].price = cur_and_len_price;
                    enc->opt_buf[offset].len = (unsigned)len_test_2;
                    enc->opt_buf[offset].dist = 0;
                    enc->opt_buf[offset].extra = 1;
                }
            }
        }
    }

    size_t max_length = MIN(bytes_avail, fast_length);
    size_t start_len = 2;

    if (match.length > 0) {
        size_t len_test;
        size_t len;
        U32 cur_rep_price;
        for (size_t rep_index = 0; rep_index < kNumReps; ++rep_index) {
            const BYTE *data_2 = data - reps[rep_index] - 1;
            if (MEM_read16(data) != MEM_read16(data_2))
                continue;
            len_test = ZSTD_count(data + 2, data_2 + 2, data + max_length) + 2;
            len_end = MAX(len_end, cur + len_test);
            cur_rep_price = rep_match_price + LZMA_getRepPrice(enc, rep_index, state, pos_state);
            len = 2;
            /* Try rep match */
            do {
                U32 cur_and_len_price = cur_rep_price + enc->states.rep_len_states.prices[pos_state][len - kMatchLenMin];
                OptimalNode* opt = &enc->opt_buf[cur + len];
                if (cur_and_len_price < opt->price) {
                    opt->price = cur_and_len_price;
                    opt->len = (unsigned)len;
                    opt->dist = (U32)(rep_index);
                    opt->extra = 0;
                }
            } while (++len <= len_test);

            if (rep_index == 0) {
                /* Save time by exluding normal matches not longer than the rep */
                start_len = len_test + 1;
            }
            if (is_hybrid && len_test + 3 <= bytes_avail && MEM_read16(data + len_test + 1) == MEM_read16(data_2 + len_test + 1)) {
                /* Try rep + literal + rep0 */
                size_t len_test_2 = ZSTD_count(data + len_test + 3,
                    data_2 + len_test + 3,
                    data + MIN(len_test + 1 + fast_length, bytes_avail)) + 2;
                size_t state_2 = RepNextState(state);
                size_t pos_state_next = (index + len_test) & pos_mask;
                U32 rep_lit_rep_total_price =
                    cur_rep_price + enc->states.rep_len_states.prices[pos_state][len_test - kMatchLenMin]
                    + GET_PRICE_0(enc->states.is_match[state_2][pos_state_next])
                    + LZMA_getLiteralPriceMatched(GetLiteralProbs(enc, index + len_test, data[len_test - 1]),
                        data[len_test], data_2[len_test]);
                size_t offset;

                state_2 = kState_LitAfterRep;
                pos_state_next = (index + len_test + 1) & pos_mask;
                rep_lit_rep_total_price +=
                    GET_PRICE_1(enc->states.is_match[state_2][pos_state_next]) +
                    GET_PRICE_1(enc->states.is_rep[state_2]);
                offset = cur + len_test + 1 + len_test_2;
                rep_lit_rep_total_price += LZMA_getRepMatch0Price(enc, len_test_2, state_2, pos_state_next);
                if (rep_lit_rep_total_price < enc->opt_buf[offset].price) {
                    len_end = MAX(len_end, offset);
                    enc->opt_buf[offset].price = rep_lit_rep_total_price;
                    enc->opt_buf[offset].len = (unsigned)len_test_2;
                    enc->opt_buf[offset].dist = (U32)(rep_index);
                    enc->opt_buf[offset].extra = (unsigned)(len_test + 1);
                }
            }
        }
    }
    if (match.length >= start_len && max_length >= start_len) {
        /* Try normal match */
        U32 normal_match_price = match_price + GET_PRICE_0(is_rep_prob);
        if (!is_hybrid) {
            /* Normal mode - single match */
            size_t length = MIN(match.length, max_length);
            size_t cur_dist = match.dist;
            size_t dist_slot = LZMA_getDistSlot(match.dist);
            size_t len_test = length;
            len_end = MAX(len_end, cur + length);
            for (; len_test >= start_len; --len_test) {
                OptimalNode *opt;
                U32 cur_and_len_price = normal_match_price + enc->states.len_states.prices[pos_state][len_test - kMatchLenMin];
                size_t len_to_dist_state = GetLenToDistState(len_test);

                if (cur_dist < kNumFullDistances) {
                    cur_and_len_price += enc->distance_prices[len_to_dist_state][cur_dist];
                }
                else {
                    cur_and_len_price += enc->dist_slot_prices[len_to_dist_state][dist_slot] + enc->align_prices[cur_dist & kAlignMask];
                }
                opt = &enc->opt_buf[cur + len_test];
                if (cur_and_len_price < opt->price) {
                    opt->price = cur_and_len_price;
                    opt->len = (unsigned)len_test;
                    opt->dist = (U32)(cur_dist + kNumReps);
                    opt->extra = 0;
                }
                else break;
            }
        }
        else {
            /* Hybrid mode */
            size_t main_len;
            ptrdiff_t match_index;
            ptrdiff_t start_match;

            match.length = MIN(match.length, (U32)max_length);
            if (match.length < 3 || max_length < 4) {
                enc->matches[0] = match;
                enc->match_count = 1;
                main_len = match.length;
            }
            else {
                main_len = LZMA_hashGetMatches(enc, block, index, max_length, match);
            }
            match_index = enc->match_count - 1;
            len_end = MAX(len_end, cur + main_len);
            start_match = 0;
            while (start_len > enc->matches[start_match].length) {
                ++start_match;
            }
            for (; match_index >= start_match; --match_index) {
                size_t len_test = enc->matches[match_index].length;
                size_t cur_dist = enc->matches[match_index].dist;
                size_t dist_slot = LZMA_getDistSlot((U32)cur_dist);
                U32 cur_and_len_price;
                size_t base_len = (match_index > start_match) ? enc->matches[match_index - 1].length + 1 : start_len;
                /* Pre-load rep0 data bytes */
                unsigned rep_0_bytes = MEM_read16(data - cur_dist + len_test);
                for (; len_test >= base_len; --len_test) {
                    size_t len_to_dist_state;
                    OptimalNode *opt;

                    cur_and_len_price = normal_match_price + enc->states.len_states.prices[pos_state][len_test - kMatchLenMin];
                    len_to_dist_state = GetLenToDistState(len_test);
                    if (cur_dist < kNumFullDistances) {
                        cur_and_len_price += enc->distance_prices[len_to_dist_state][cur_dist];
                    }
                    else {
                        cur_and_len_price += enc->dist_slot_prices[len_to_dist_state][dist_slot] + enc->align_prices[cur_dist & kAlignMask];
                    }
                    opt = &enc->opt_buf[cur + len_test];
                    if (cur_and_len_price < opt->price) {
                        opt->price = cur_and_len_price;
                        opt->len = (unsigned)len_test;
                        opt->dist = (U32)(cur_dist + kNumReps);
                        opt->extra = 0;
                    }
                    else if(len_test < main_len)
                        break;
                    if (len_test == enc->matches[match_index].length) {
                        size_t rep_0_pos = len_test + 1;
                        if (rep_0_pos + 2 <= bytes_avail && rep_0_bytes == MEM_read16(data + rep_0_pos)) {
                            /* Try match + literal + rep0 */
                            const BYTE *data_2 = data - cur_dist - 1;
                            size_t limit = MIN(rep_0_pos + fast_length, bytes_avail);
                            size_t len_test_2 = ZSTD_count(data + rep_0_pos + 2, data_2 + rep_0_pos + 2, data + limit) + 2;
                            size_t state_2 = MatchNextState(state);
                            size_t pos_state_next = (index + len_test) & pos_mask;
                            U32 match_lit_rep_total_price = cur_and_len_price +
                                GET_PRICE_0(enc->states.is_match[state_2][pos_state_next]) +
                                LZMA_getLiteralPriceMatched(GetLiteralProbs(enc, index + len_test, data[len_test - 1]),
                                    data[len_test], data_2[len_test]);

                            state_2 = kState_LitAfterMatch;
                            pos_state_next = (pos_state_next + 1) & pos_mask;
                            match_lit_rep_total_price +=
                                GET_PRICE_1(enc->states.is_match[state_2][pos_state_next]) +
                                GET_PRICE_1(enc->states.is_rep[state_2]);
                            size_t offset = cur + rep_0_pos + len_test_2;
                            match_lit_rep_total_price += LZMA_getRepMatch0Price(enc, len_test_2, state_2, pos_state_next);
                            if (match_lit_rep_total_price < enc->opt_buf[offset].price) {
                                len_end = MAX(len_end, offset);
                                enc->opt_buf[offset].price = match_lit_rep_total_price;
                                enc->opt_buf[offset].len = (unsigned)len_test_2;
                                enc->opt_buf[offset].extra = (unsigned)rep_0_pos;
                                enc->opt_buf[offset].dist = (U32)(cur_dist + kNumReps);
                            }
                        }
                    }
                }
            }
        }
    }
    return len_end;
}

FORCE_NOINLINE
static void LZMA_initMatchesPos0(LZMA2_ECtx *const enc,
    RMF_match const match,
    size_t const pos_state,
    size_t len,
    unsigned const normal_match_price)
{
    if ((unsigned)len <= match.length) {
        size_t const distance = match.dist;
        size_t const slot = LZMA_getDistSlot(match.dist);
        /* Test every available length of the match */
        do
        {
            unsigned cur_and_len_price = normal_match_price + enc->states.len_states.prices[pos_state][len - kMatchLenMin];
            size_t len_to_dist_state = GetLenToDistState(len);
            if (distance < kNumFullDistances) {
                cur_and_len_price += enc->distance_prices[len_to_dist_state][distance];
            }
            else {
                cur_and_len_price += enc->align_prices[distance & kAlignMask] + enc->dist_slot_prices[len_to_dist_state][slot];
            }
            if (cur_and_len_price < enc->opt_buf[len].price) {
                enc->opt_buf[len].price = cur_and_len_price;
                enc->opt_buf[len].len = (unsigned)len;
                enc->opt_buf[len].dist = (U32)(distance + kNumReps);
                enc->opt_buf[len].extra = 0;
            }
            ++len;
        } while ((unsigned)len <= match.length);
    }
}

FORCE_NOINLINE
static size_t LZMA_initMatchesPos0Best(LZMA2_ECtx *const enc, FL2_dataBlock const block,
    RMF_match const match,
    size_t const index,
    size_t len,
    unsigned const normal_match_price)
{
    if (len <= match.length) {
        size_t main_len;
        if (match.length < 3 || block.end - index < 4) {
            enc->matches[0] = match;
            enc->match_count = 1;
            main_len = match.length;
        }
        else {
            main_len = LZMA_hashGetMatches(enc, block, index, MIN(block.end - index, enc->fast_length), match);
        }

        size_t match_index = 0;
        while (len > enc->matches[match_index].length)
            ++match_index;

        size_t pos_state = index & enc->pos_mask;
        size_t distance = enc->matches[match_index].dist;
        size_t slot = LZMA_getDistSlot(enc->matches[match_index].dist);
        /* Test every available match length at the shortest distance. The buffer is sorted */
        /* in order of increasing length, and therefore increasing distance too. */
        for (;; ++len) {
            unsigned cur_and_len_price = normal_match_price
                + enc->states.len_states.prices[pos_state][len - kMatchLenMin];
            size_t len_to_dist_state = GetLenToDistState(len);
            if (distance < kNumFullDistances) {
                cur_and_len_price += enc->distance_prices[len_to_dist_state][distance];
            }
            else {
                cur_and_len_price += enc->align_prices[distance & kAlignMask] + enc->dist_slot_prices[len_to_dist_state][slot];
            }
            if (cur_and_len_price < enc->opt_buf[len].price) {
                enc->opt_buf[len].price = cur_and_len_price;
                enc->opt_buf[len].len = (unsigned)len;
                enc->opt_buf[len].dist = (U32)(distance + kNumReps);
                enc->opt_buf[len].extra = 0;
            }
            if (len == enc->matches[match_index].length) {
                /* Run out of length for this match. Get the next if any. */
                if (len == main_len) {
                    break;
                }
                ++match_index;
                distance = enc->matches[match_index].dist;
                slot = LZMA_getDistSlot(enc->matches[match_index].dist);
            }
        }
        return main_len;
    }
    return 0;
}

/* Test all available options at position 0 of the optimizer buffer.
* The prices at this point are all initialized to kInfinityPrice.
* This function must not be called at a position where no match is
* available. */
FORCE_INLINE_TEMPLATE
size_t LZMA_initOptimizerPos0(LZMA2_ECtx *const enc, FL2_dataBlock const block,
    RMF_match const match,
    size_t const index,
    int const is_hybrid,
    U32* const reps)
{
    size_t const max_length = MIN(block.end - index, kMatchLenMax);
    const BYTE *const data = block.data + index;
    const BYTE *data_2;
    size_t rep_max_index = 0;
    size_t rep_lens[kNumReps];

    /* Find any rep matches */
    for (size_t i = 0; i < kNumReps; ++i) {
        reps[i] = enc->states.reps[i];
        data_2 = data - reps[i] - 1;
        if (MEM_read16(data) != MEM_read16(data_2)) {
            rep_lens[i] = 0;
            continue;
        }
        rep_lens[i] = ZSTD_count(data + 2, data_2 + 2, data + max_length) + 2;
        if (rep_lens[i] > rep_lens[rep_max_index])
            rep_max_index = i;
    }
    if (rep_lens[rep_max_index] >= enc->fast_length) {
        enc->opt_buf[0].len = (unsigned)(rep_lens[rep_max_index]);
        enc->opt_buf[0].dist = (U32)rep_max_index;
        return 0;
    }
    if (match.length >= enc->fast_length) {
        enc->opt_buf[0].len = match.length;
        enc->opt_buf[0].dist = match.dist + kNumReps;
        return 0;
    }

    unsigned const cur_byte = *data;
    unsigned const match_byte = *(data - reps[0] - 1);
    size_t const state = enc->states.state;
    size_t const pos_state = index & enc->pos_mask;
    Probability const is_match_prob = enc->states.is_match[state][pos_state];
    Probability const is_rep_prob = enc->states.is_rep[state];

    enc->opt_buf[0].state = state;
    /* Set the price for literal */
    enc->opt_buf[1].price = GET_PRICE_0(is_match_prob) +
        LZMA_getLiteralPrice(enc, index, state, data[-1], cur_byte, match_byte);
    MakeAsLiteral(enc->opt_buf[1]);

    unsigned match_price = GET_PRICE_1(is_match_prob);
    unsigned rep_match_price = match_price + GET_PRICE_1(is_rep_prob);
    if (match_byte == cur_byte) {
        /* Try 1-byte rep0 */
        unsigned short_rep_price = rep_match_price + LZMA_getRepLen1Price(enc, state, pos_state);
        if (short_rep_price < enc->opt_buf[1].price) {
            enc->opt_buf[1].price = short_rep_price;
            MakeAsShortRep(enc->opt_buf[1]);
        }
    }
    memcpy(enc->opt_buf[0].reps, reps, sizeof(enc->opt_buf[0].reps));
    enc->opt_buf[1].len = 1;
    /* Test the rep match prices */
    for (size_t i = 0; i < kNumReps; ++i) {
        unsigned price;
        size_t rep_len = rep_lens[i];
        if (rep_len < 2) {
            continue;
        }
        price = rep_match_price + LZMA_getRepPrice(enc, i, state, pos_state);
        /* Test every available length of the rep */
        do {
            unsigned cur_and_len_price = price + enc->states.rep_len_states.prices[pos_state][rep_len - kMatchLenMin];
            if (cur_and_len_price < enc->opt_buf[rep_len].price) {
                enc->opt_buf[rep_len].price = cur_and_len_price;
                enc->opt_buf[rep_len].len = (unsigned)rep_len;
                enc->opt_buf[rep_len].dist = (U32)i;
                enc->opt_buf[rep_len].extra = 0;
            }
        } while (--rep_len >= kMatchLenMin);
    }
    unsigned normal_match_price = match_price + GET_PRICE_0(is_rep_prob);
    size_t len = (rep_lens[0] >= 2) ? rep_lens[0] + 1 : 2;
    /* Test the match prices */
    if (!is_hybrid) {
        /* Normal mode */
        LZMA_initMatchesPos0(enc, match, pos_state, len, normal_match_price);
        return MAX(match.length, rep_lens[rep_max_index]);
    }
    else {
        /* Hybrid mode */
        size_t main_len = LZMA_initMatchesPos0Best(enc, block, match, index, len, normal_match_price);
        return MAX(main_len, rep_lens[rep_max_index]);
    }
}

FORCE_INLINE_TEMPLATE
size_t LZMA_encodeOptimumSequence(LZMA2_ECtx *const enc, FL2_dataBlock const block,
    FL2_matchTable* const tbl,
    int const struct_tbl,
    int const is_hybrid,
    size_t start_index,
    size_t const uncompressed_end,
    RMF_match match)
{
    size_t len_end = enc->len_end_max;
    unsigned const search_depth = tbl->params.depth;
    do {
        size_t const pos_mask = enc->pos_mask;

        for (; (len_end & 3) != 0; --len_end)
            enc->opt_buf[len_end].price = kInfinityPrice;

        for (; len_end >= 4; len_end -= 4) {
            enc->opt_buf[len_end].price = kInfinityPrice;
            enc->opt_buf[len_end - 1].price = kInfinityPrice;
            enc->opt_buf[len_end - 2].price = kInfinityPrice;
            enc->opt_buf[len_end - 3].price = kInfinityPrice;
        }

        /* Set everything up at position 0 */
        size_t index = start_index;
        U32 reps[kNumReps];
        len_end = LZMA_initOptimizerPos0(enc, block, match, index, is_hybrid, reps);
        match.length = 0;
        size_t cur = 1;

        /* len_end == 0 if a match of fast_length was found */
        if (len_end > 0) {
            ++index;
            for (; cur < len_end; ++cur, ++index) {

                if (cur >= kOptimizerBufferSize - kOptimizerEndSize) {
                    size_t j, best;
                    U32 price = enc->opt_buf[cur].price;
                    U32 delta = price / (U32)cur / 2U;
                    best = cur;
                    for (j = cur + 1; j <= len_end; j++) {
                        U32 price2 = enc->opt_buf[j].price;
                        if (price >= price2) {
                            price = price2;
                            best = j;
                        }
                        price += delta;
                    }
                    cur = best;
                    break;
                }

                size_t end = MIN(cur + 4, len_end);
                U32 price = enc->opt_buf[cur].price;
                for (size_t j = cur + 1; j <= end; j++) {
                    U32 price2 = enc->opt_buf[j].price;
                    if (price >= price2) {
                        price = price2;
                        index += j - cur;
                        cur = j;
                        if (cur == len_end)
                            goto reverse;
                    }
                }
#if 0
                if (enc->opt_buf[cur + 1].price < enc->opt_buf[cur].price)
                    continue;
#endif
                match = RMF_getMatch(block, tbl, search_depth, struct_tbl, index);
                if (match.length >= enc->fast_length)
                    break;

                len_end = LZMA_optimalParse(enc, block, match, index, cur, len_end, is_hybrid, reps);
            }
reverse:
            DEBUGLOG(6, "End optimal parse at %u", (U32)cur);
            LZMA_reverseOptimalChain(enc->opt_buf, cur);
        }
        /* Encode the selections in the buffer */
        size_t i = 0;
        do {
            unsigned len = enc->opt_buf[i].len;

            if (len == 1 && enc->opt_buf[i].dist == kNullDist) {
                LZMA_encodeLiteralBuf(enc, block.data, start_index + i);
            }
            else {
                size_t match_index = start_index + i;
                U32 dist = enc->opt_buf[i].dist;
#if 0
                /* The last match will be truncated to fit in the optimal buffer so get the full length */
                if (i + len >= kOptimizerBufferSize - 1 && dist >= kNumReps) {
                    RMF_match lastmatch = RMF_getMatch(block, tbl, search_depth, struct_tbl, match_index);
                    if (lastmatch.length > len) {
                        len = lastmatch.length;
                        dist = lastmatch.dist + kNumReps;
                    }
                }
#endif
                if (dist >= kNumReps) 
                    LZMA_encodeNormalMatch(enc, len, dist - kNumReps, match_index & pos_mask);
                else
                    LZMA_encodeRepMatch(enc, len, dist, match_index & pos_mask);
            }
            i += len;
        } while (i < cur);
        start_index += i;
        /* Do another round if there is a long match pending, because the reps must be checked */
        /* and the match encoded. */
    } while (match.length >= enc->fast_length && start_index < uncompressed_end && enc->rc.out_index < enc->rc.chunk_size);
    enc->len_end_max = len_end;
    return start_index;
}

static void FORCE_NOINLINE LZMA_fillAlignPrices(LZMA2_ECtx *const enc)
{
    unsigned i;
    const Probability *probs = enc->states.dist_align_encoders;
    for (i = 0; i < kAlignTableSize / 2; i++) {
        U32 price = 0;
        unsigned sym = i;
        unsigned m = 1;
        unsigned bit;
        U32 prob;
        bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[m], bit); m = (m << 1) + bit;
        bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[m], bit); m = (m << 1) + bit;
        bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[m], bit); m = (m << 1) + bit;
        prob = probs[m];
        enc->align_prices[i] = price + GET_PRICE_0(prob);
        enc->align_prices[i + 8] = price + GET_PRICE_1(prob);
    }
}


static void FORCE_NOINLINE LZMA_fillDistancesPrices(LZMA2_ECtx *const enc)
{
    U32 * const temp_prices = enc->distance_prices[kNumLenToPosStates - 1];

    enc->match_price_count = 0;

    for (size_t i = kStartPosModelIndex / 2; i < kNumFullDistances / 2; i++)
    {
        unsigned dist_slot = distance_table[i];
        unsigned footer_bits = (dist_slot >> 1) - 1;
        size_t base = ((2 | (dist_slot & 1)) << footer_bits);
        const Probability *probs = enc->states.dist_encoders + base * 2U;
        base += i;
        probs = probs - distance_table[base] - 1;
        U32 price = 0;
        unsigned m = 1;
        unsigned sym = (unsigned)i;
        unsigned offset = (unsigned)1 << footer_bits;

        for (; footer_bits != 0; --footer_bits) {
            unsigned bit = sym & 1;
            sym >>= 1;
            price += GET_PRICE(probs[m], bit);
            m = (m << 1) + bit;
        };

        unsigned prob = probs[m];
        temp_prices[base] = price + GET_PRICE_0(prob);
        temp_prices[base + offset] = price + GET_PRICE_1(prob);
    }

    for (unsigned lps = 0; lps < kNumLenToPosStates; lps++) {
        size_t slot;
        size_t dist_table_size2 = (enc->dist_price_table_size + 1) >> 1;
        U32 *dist_slot_prices = enc->dist_slot_prices[lps];
        const Probability *probs = enc->states.dist_slot_encoders[lps];

        for (slot = 0; slot < dist_table_size2; slot++) {
            /* dist_slot_prices[slot] = RcTree_GetPrice(encoder, kNumPosSlotBits, slot, p->ProbPrices); */
            U32 price;
            unsigned bit;
            unsigned sym = (unsigned)slot + (1 << (kNumPosSlotBits - 1));
            bit = sym & 1; sym >>= 1; price = GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            unsigned prob = probs[slot + (1 << (kNumPosSlotBits - 1))];
            dist_slot_prices[slot * 2] = price + GET_PRICE_0(prob);
            dist_slot_prices[slot * 2 + 1] = price + GET_PRICE_1(prob);
        }

        {
            U32 delta = ((U32)((kEndPosModelIndex / 2 - 1) - kNumAlignBits) << kNumBitPriceShiftBits);
            for (slot = kEndPosModelIndex / 2; slot < dist_table_size2; slot++) {
                dist_slot_prices[slot * 2] += delta;
                dist_slot_prices[slot * 2 + 1] += delta;
                delta += ((U32)1 << kNumBitPriceShiftBits);
            }
        }

        {
            U32 *dp = enc->distance_prices[lps];

            dp[0] = dist_slot_prices[0];
            dp[1] = dist_slot_prices[1];
            dp[2] = dist_slot_prices[2];
            dp[3] = dist_slot_prices[3];

            for (size_t i = 4; i < kNumFullDistances; i += 2) {
                U32 slot_price = dist_slot_prices[distance_table[i]];
                dp[i] = slot_price + temp_prices[i];
                dp[i + 1] = slot_price + temp_prices[i + 1];
            }
        }
    }
}

FORCE_INLINE_TEMPLATE
size_t LZMA_encodeChunkBest(LZMA2_ECtx *const enc,
    FL2_dataBlock const block,
    FL2_matchTable* const tbl,
    int const struct_tbl,
    size_t index,
    size_t const uncompressed_end)
{
    unsigned const search_depth = tbl->params.depth;
    LZMA_fillDistancesPrices(enc);
    LZMA_fillAlignPrices(enc);
    LZMA_lengthStates_updatePrices(enc, &enc->states.len_states);
    LZMA_lengthStates_updatePrices(enc, &enc->states.rep_len_states);
    while (index < uncompressed_end && enc->rc.out_index < enc->rc.chunk_size)
    {
        RMF_match const match = RMF_getMatch(block, tbl, search_depth, struct_tbl, index);
        if (match.length > 1) {
            if (enc->strategy != FL2_ultra) {
                index = LZMA_encodeOptimumSequence(enc, block, tbl, struct_tbl, 0, index, uncompressed_end, match);
            }
            else {
                index = LZMA_encodeOptimumSequence(enc, block, tbl, struct_tbl, 1, index, uncompressed_end, match);
            }
        }
        else {
            if (block.data[index] == block.data[index - enc->states.reps[0] - 1]) {
                LZMA_encodeRepMatch(enc, 1, 0, index & enc->pos_mask);
            }
            else {
                LZMA_encodeLiteralBuf(enc, block.data, index);
            }
            ++index;
        }
        if (enc->match_price_count >= kMatchRepriceFrequency) {
            LZMA_fillAlignPrices(enc);
            LZMA_fillDistancesPrices(enc);
            LZMA_lengthStates_updatePrices(enc, &enc->states.len_states);
        }
        if (enc->rep_len_price_count >= kRepLenRepriceFrequency) {
            enc->rep_len_price_count = 0;
            LZMA_lengthStates_updatePrices(enc, &enc->states.rep_len_states);
        }
    }
    return index;
}

static void LZMA_lengthStates_Reset(LengthStates* const ls, unsigned const fast_length)
{
    ls->choice = kProbInitValue;
    for (size_t i = 0; i < (kNumPositionStatesMax << (kLenNumLowBits + 1)); ++i) {
        ls->low[i] = kProbInitValue;
    }
    for (size_t i = 0; i < kLenNumHighSymbols; ++i) {
        ls->high[i] = kProbInitValue;
    }
    ls->table_size = fast_length + 1 - kMatchLenMin;
}

static void LZMA_encoderStates_Reset(EncoderStates* const es, unsigned const lc, unsigned const lp, unsigned fast_length)
{
    es->state = 0;
    for (size_t i = 0; i < kNumReps; ++i) {
        es->reps[i] = 0;
    }
    for (size_t i = 0; i < kNumStates; ++i) {
        for (size_t j = 0; j < kNumPositionStatesMax; ++j) {
            es->is_match[i][j] = kProbInitValue;
            es->is_rep0_long[i][j] = kProbInitValue;
        }
        es->is_rep[i] = kProbInitValue;
        es->is_rep_G0[i] = kProbInitValue;
        es->is_rep_G1[i] = kProbInitValue;
        es->is_rep_G2[i] = kProbInitValue;
    }
    size_t const num = (size_t)(kNumLiterals * kNumLitTables) << (lp + lc);
    for (size_t i = 0; i < num; ++i) {
        es->literal_probs[i] = kProbInitValue;
    }
    for (size_t i = 0; i < kNumLenToPosStates; ++i) {
        Probability *probs = es->dist_slot_encoders[i];
        for (size_t j = 0; j < (1 << kNumPosSlotBits); ++j) {
            probs[j] = kProbInitValue;
        }
    }
    for (size_t i = 0; i < kNumFullDistances - kEndPosModelIndex; ++i) {
        es->dist_encoders[i] = kProbInitValue;
    }
    LZMA_lengthStates_Reset(&es->len_states, fast_length);
    LZMA_lengthStates_Reset(&es->rep_len_states, fast_length);
    for (size_t i = 0; i < (1 << kNumAlignBits); ++i) {
        es->dist_align_encoders[i] = kProbInitValue;
    }
}

BYTE LZMA2_getDictSizeProp(size_t const dictionary_size)
{
    BYTE dict_size_prop = 0;
    for (BYTE bit = 11; bit < 32; ++bit) {
        if (((size_t)2 << bit) >= dictionary_size) {
            dict_size_prop = (bit - 11) << 1;
            break;
        }
        if (((size_t)3 << bit) >= dictionary_size) {
            dict_size_prop = ((bit - 11) << 1) | 1;
            break;
        }
    }
    return dict_size_prop;
}

size_t LZMA2_encMemoryUsage(unsigned const chain_log, FL2_strategy const strategy, unsigned const thread_count)
{
    size_t size = sizeof(LZMA2_ECtx);
    if(strategy == FL2_ultra)
        size += sizeof(HashChains) + (sizeof(U32) << chain_log) - sizeof(U32);
    return size * thread_count;
}

static void LZMA2_reset(LZMA2_ECtx *const enc, size_t const max_distance)
{
    DEBUGLOG(5, "LZMA encoder reset : max_distance %u", (unsigned)max_distance);
    RC_reset(&enc->rc);
    LZMA_encoderStates_Reset(&enc->states, enc->lc, enc->lp, enc->fast_length);
    enc->pos_mask = (1 << enc->pb) - 1;
    enc->lit_pos_mask = (1 << enc->lp) - 1;
    U32 i = 0;
    for (; max_distance > (size_t)1 << i; ++i) {
    }
    enc->dist_price_table_size = i * 2;
    enc->rep_len_price_count = 0;
    enc->match_price_count = 0;
}

static BYTE LZMA_getLcLpPbCode(LZMA2_ECtx *const enc)
{
    return (BYTE)((enc->pb * 5 + enc->lp) * 9 + enc->lc);
}

/* Integer square root from https://stackoverflow.com/a/1101217 */
static U32 LZMA2_isqrt(U32 op)
{
    U32 res = 0;
    /* "one" starts at the highest power of four <= than the argument. */
    U32 one = (U32)1 << (ZSTD_highbit32(op) & ~1);

    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = res + 2U * one;
        }
        res >>= 1;
        one >>= 2;
    }
    return res;
}

static BYTE LZMA2_isChunkCompressible(const FL2_matchTable* const tbl,
    FL2_dataBlock const block, size_t const start,
	unsigned const strategy)
{
	if (block.end - start >= kMinTestChunkSize) {
		static const size_t max_dist_table[][5] = {
			{ 0, 0, 0, 1U << 6, 1U << 14 }, /* fast */
			{ 0, 0, 1U << 6, 1U << 14, 1U << 22 }, /* opt */
			{ 0, 0, 1U << 6, 1U << 14, 1U << 22 } }; /* ultra */
		static const size_t margin_divisor[3] = { 60U, 45U, 120U };
		static const U32 dev_table[3] = { 24, 24, 20};

		size_t const end = MIN(start + kChunkSize, block.end);
		size_t const chunk_size = end - start;
		size_t count = 0;
		size_t const margin = chunk_size / margin_divisor[strategy];
		size_t const terminator = start + margin;

		if (tbl->is_struct) {
			size_t prev_dist = 0;
			for (size_t index = start; index < end; ) {
				U32 const link = GetMatchLink(tbl->table, index);
				if (link == RADIX_NULL_LINK) {
					++index;
					++count;
					prev_dist = 0;
				}
				else {
					size_t length = GetMatchLength(tbl->table, index);
					size_t dist = index - GetMatchLink(tbl->table, index);
					if (length > 4)
						count += dist != prev_dist;
					else
						count += (dist < max_dist_table[strategy][length]) ? 1 : length;
					index += length;
					prev_dist = dist;
				}
				if (count + terminator <= index)
					return 0;
			}
		}
		else {
			size_t prev_dist = 0;
			for (size_t index = start; index < end; ) {
				U32 const link = tbl->table[index];
				if (link == RADIX_NULL_LINK) {
					++index;
					++count;
					prev_dist = 0;
				}
				else {
					size_t length = link >> RADIX_LINK_BITS;
					size_t dist = index - (link & RADIX_LINK_MASK);
					if (length > 4)
						count += dist != prev_dist;
					else
						count += (dist < max_dist_table[strategy][length]) ? 1 : length;
					index += length;
					prev_dist = dist;
				}
				if (count + terminator <= index)
					return 0;
			}
		}

        U32 char_count[256];
        U32 char_total = 0;
        /* Expected normal character count */
        U32 const avg = (U32)(chunk_size / 64U);

        memset(char_count, 0, sizeof(char_count));
        for (size_t index = start; index < end; ++index)
            char_count[block.data[index]] += 4;
        /* Sum the deviations */
        for (size_t i = 0; i < 256; ++i) {
            S32 delta = char_count[i] - avg;
            char_total += delta * delta;
        }
        U32 sqrt_chunk = (chunk_size == kChunkSize) ? kSqrtChunkSize : LZMA2_isqrt((U32)chunk_size);
        return LZMA2_isqrt(char_total) / sqrt_chunk <= dev_table[strategy];
	}
	return 0;
}

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#else
__pragma(warning(disable:4701))
#endif

static size_t LZMA2_encodeChunk(LZMA2_ECtx *const enc,
    FL2_matchTable* const tbl,
    FL2_dataBlock const block,
    size_t const index, size_t const end)
{
    if (enc->strategy == FL2_fast) {
        if (tbl->is_struct) {
            return LZMA_encodeChunkFast(enc, block, tbl, 1,
                index, end);
        }
        else {
            return LZMA_encodeChunkFast(enc, block, tbl, 0,
                index, end);
        }
    }
    else {
        if (tbl->is_struct) {
            return LZMA_encodeChunkBest(enc, block, tbl, 1,
                index, end);
        }
        else {
            return LZMA_encodeChunkBest(enc, block, tbl, 0,
                index, end);
        }
    }

}

size_t LZMA2_encode(LZMA2_ECtx *const enc,
    FL2_matchTable* const tbl,
    FL2_dataBlock const block,
    const FL2_lzma2Parameters* const options,
    int stream_prop,
    FL2_atomic *const progress_in,
    FL2_atomic *const progress_out,
    int *const canceled)
{
    size_t const start = block.start;
    BYTE* out_dest = enc->out_buf;
	/* Each encoder writes a properties byte because the upstream encoder(s) could */
	/* write only uncompressed chunks with no properties. */
	BYTE encode_properties = 1;
    BYTE next_is_random = 0;

    if (block.end <= block.start)
        return 0;

    enc->lc = options->lc;
    enc->lp = MIN(options->lp, 4);

    if (enc->lc + enc->lp > 4)
        enc->lc = 4 - enc->lp;

    enc->pb = options->pb;
    enc->strategy = options->strategy;
    enc->fast_length = options->fast_length;
    enc->match_cycles = options->match_cycles;

    LZMA2_reset(enc, block.end);

    if (enc->strategy == FL2_ultra) {
        /* Create a hash chain to put the encoder into hybrid mode */
        if (enc->hash_alloc_3 < ((ptrdiff_t)1 << options->second_dict_bits)) {
            if(LZMA_hashCreate(enc, options->second_dict_bits) != 0)
                return FL2_ERROR(memory_allocation);
        }
        else {
            LZMA_hashReset(enc, options->second_dict_bits);
        }
        enc->hash_prev_index = (start >= (size_t)enc->hash_dict_3) ? (ptrdiff_t)(start - enc->hash_dict_3) : (ptrdiff_t)-1;
    }
    enc->len_end_max = kOptimizerBufferSize - 1;
    RMF_limitLengths(tbl, block.end);
    for (size_t index = start; index < block.end;)
    {
        size_t header_size = (stream_prop >= 0) + (encode_properties ? kChunkHeaderSize + 1 : kChunkHeaderSize);
        EncoderStates saved_states;
        size_t next_index;
        RC_reset(&enc->rc);
        RC_setOutputBuffer(&enc->rc, out_dest + header_size, kChunkSize);
        if (!next_is_random) {
            size_t cur = index;
            size_t end;
            if (enc->strategy == FL2_fast)
                end = MIN(block.end, index + kMaxChunkUncompressedSize);
            else
                end = MIN(block.end, index + kMaxChunkUncompressedSize - kOptimizerBufferSize);
            saved_states = enc->states;
            if (index == 0) {
                LZMA_encodeLiteral(enc, 0, block.data[0], 0);
                ++cur;
            }
            if (index == start) {
                /* After four bytes we can write data to the match table because the */
                /* compressed data will never catch up with the table position being read. */
                enc->rc.chunk_size = kTempMinOutput;
                cur = LZMA2_encodeChunk(enc, tbl, block, cur, end);
                enc->rc.chunk_size = kChunkSize;
                out_dest = RMF_getTableAsOutputBuffer(tbl, start);
                memcpy(out_dest, enc->out_buf, header_size + enc->rc.out_index);
                enc->rc.out_buffer = out_dest + header_size;
            }
            next_index = LZMA2_encodeChunk(enc, tbl, block, cur, end);
            RC_flush(&enc->rc);
        }
        else {
            next_index = MIN(index + kChunkSize, block.end);
        }
        size_t compressed_size = enc->rc.out_index;
        size_t uncompressed_size = next_index - index;
        if (compressed_size > kMaxChunkCompressedSize)
            return FL2_ERROR(internal);
        BYTE* header = out_dest;
        if (stream_prop >= 0)
            *header++ = (BYTE)stream_prop;
        stream_prop = -1;
        header[1] = (BYTE)((uncompressed_size - 1) >> 8);
        header[2] = (BYTE)(uncompressed_size - 1);
        /* Output an uncompressed chunk if necessary */
        if (next_is_random || uncompressed_size + 3 <= compressed_size + header_size) {
            DEBUGLOG(5, "Storing chunk : was %u => %u", (unsigned)uncompressed_size, (unsigned)compressed_size);
            if (index == 0) {
                header[0] = kChunkUncompressedDictReset;
            }
            else {
                header[0] = kChunkUncompressed;
            }
            memcpy(header + 3, block.data + index, uncompressed_size);
            compressed_size = uncompressed_size;
            header_size = 3 + (header - out_dest);
            if (!next_is_random) {
                enc->states = saved_states;
            }
        }
        else {
            DEBUGLOG(5, "Compressed chunk : %u => %u", (unsigned)uncompressed_size, (unsigned)compressed_size);
            if (index == 0) {
                header[0] = kChunkCompressedFlag | kChunkAllReset;
            }
            else if (encode_properties) {
                header[0] = kChunkCompressedFlag | kChunkStatePropertiesReset;
            }
            else {
                header[0] = kChunkCompressedFlag | kChunkNothingReset;
            }
            header[0] |= (BYTE)((uncompressed_size - 1) >> 16);
            header[3] = (BYTE)((compressed_size - 1) >> 8);
            header[4] = (BYTE)(compressed_size - 1);
            if (encode_properties) {
                header[5] = LZMA_getLcLpPbCode(enc);
                encode_properties = 0;
            }
        }
        if (next_is_random || uncompressed_size + 3 <= compressed_size + (compressed_size >> kRandomFilterMarginBits) + header_size)
        {
            /* Test the next chunk for compressibility */
            next_is_random = LZMA2_isChunkCompressible(tbl, block, next_index, enc->strategy);
        }
        out_dest += compressed_size + header_size;
        FL2_atomic_add(*progress_in, (long)(next_index - index));
        FL2_atomic_add(*progress_out, (long)(compressed_size + header_size));
        index = next_index;
        if (*canceled)
            return FL2_ERROR(canceled);
    }
    return out_dest - RMF_getTableAsOutputBuffer(tbl, start);
}
