/*
 * mcu-max
 * Chess game engine for low-resource MCUs
 *
 * (C) 2022-2026 Gissio, modified by user
 *
 * License: MIT
 *
 * Based on micro-Max 4.8 by H.G. Muller.
 *
 * Engine-layer features (migrated from the app layer):
 * - Check / checkmate / stalemate detection
 * - Castling rights tracking & legality validation
 * - En passant target tracking & detection
 * - 50-move rule (halfmove clock)
 * - Threefold repetition (hash history)
 * - Insufficient material / only-kings-left draw detection
 * - Legal move filtering (rejects moves leaving own king in check)
 * - Stable board snapshot for queries during search (no shadow board
 *   needed in the app layer anymore)
 * - Game state evaluation (active / checkmate / stalemate / draw variants)
 *
 * New feature:
 * - Autonomous promotion-piece selection integrated into
 *   mcumax_search_best_move(): when the engine's best move is a pawn
 *   promotion, all four promotion pieces (Q, R, B, N) are evaluated
 *   with a shallow negamax search and the best one is returned inside
 *   mcumax_move.promotion_piece. mcumax_play_move() applies it
 *   automatically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcu-max.h"
#include <esp_random.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Configuration (force hashing for repetition detection) */
#define MCUMAX_HASHING_ENABLED

/* Constants */
#define MCUMAX_BOARD_MASK 0x88
#define MCUMAX_BOARD_WHITE 0x8
#define MCUMAX_BOARD_BLACK 0x10
#define MCUMAX_PIECE_MOVED 0x20
#define MCUMAX_SCORE_MAX 8000
#define MCUMAX_DEPTH_MAX 99
#define MCUMAX_DRAW_SCORE 0
#define MCUMAX_HISTORY_SIZE 4096
#define MCUMAX_NUM_CORES 2

enum mcumax_mode {
  MCUMAX_INTERNAL_NODE,
  MCUMAX_SEARCH_VALID_MOVES,
  MCUMAX_SEARCH_BEST_MOVE,
  MCUMAX_PLAY_MOVE,
};

static struct mcumax_state {
  /* Board: first half of 16x8 + dummy */
  uint8_t board[0x80 + 1];
  uint8_t current_side;

  /* Engine */
  int32_t score;
  uint8_t en_passant_square;
  int32_t non_pawn_material;

#ifdef MCUMAX_HASHING_ENABLED
  uint32_t hash_key;
  uint32_t hash_key2;
#endif

  /* Draw detection state */
  int32_t halfmove_clock;
  uint32_t history_size;
  uint32_t history_keys[MCUMAX_HISTORY_SIZE];

  /* Interface */
  uint8_t square_from; /* Selected move */
  uint8_t square_to;
  uint8_t root_best_from;
  uint8_t root_best_to;
  uint32_t last_search_node_count;

  uint32_t node_count;
  uint32_t node_max;
  uint32_t depth_max;

  bool stop_search;

  /* Extra */
  mcumax_callback user_callback;
  void *user_data;

  mcumax_move *valid_moves_buffer;
  uint32_t valid_moves_buffer_size;
  uint32_t valid_moves_num;

  /* ---- Stable snapshot (queried through the public API while a
   * search may be in progress on the working board above). ---- */
  uint8_t stable_board[0x80 + 1];
  uint8_t stable_current_side;
  uint8_t stable_en_passant_square;
  int32_t stable_halfmove_clock;
  uint8_t castling_rights; /* 4-bit mask, mirrors FEN flags */

  /* Promotion piece chosen by the last mcumax_search_best_move() call
   * when its best move was a promotion. */
  mcumax_piece last_promotion_piece;
} mcumax_instances[MCUMAX_NUM_CORES];

#define mcumax (mcumax_instances[(uint32_t)xPortGetCoreID() & 1])

static const int8_t mcumax_capture_values[] = {0, 2, 2, 7, -1, 8, 12, 23};

static const int8_t mcumax_step_vectors_indices[] = {0, 7, -1, 11, 6, 8, 3, 6};

static const int8_t mcumax_step_vectors[] = {
    /* Upstream pawn */
    -16, -15, -17, 0,
    /* Rook */
    1, 16, 0,
    /* King, queen */
    1, 16, 15, 17, 0,
    /* Knight */
    14, 18, 31, 33, 0};

static const int8_t mcumax_board_setup[] = {
    MCUMAX_ROOK, MCUMAX_KNIGHT, MCUMAX_BISHOP, MCUMAX_QUEEN,
    MCUMAX_KING, MCUMAX_BISHOP, MCUMAX_KNIGHT, MCUMAX_ROOK,
};

#ifdef MCUMAX_HASHING_ENABLED

#define MCUMAX_HASH_SCRAMBLE_TABLE_SIZE 1035
/* Reduced hash table size to fit ESP32-S3 DRAM (~48 KB) */
#define MCUMAX_HASH_TABLE_SIZE (1 << 12)

#define HashScramble(A, B)                                                     \
  *(uint32_t *)(mcumax_scramble_table + A + (B & 8) +                          \
                MCUMAX_SQUARE_INVALID * (B & 0b111))

#define Hash(A)                                                                \
  HashScramble(square_to + A, mcumax.board[square_to]) -                       \
      HashScramble(square_from + A, scan_piece) -                              \
      HashScramble(capture_square + A, capture_piece)

static uint8_t
    mcumax_scramble_table[MCUMAX_HASH_SCRAMBLE_TABLE_SIZE];

static void mcumax_recalculate_hash(void) {
  uint32_t key = 0;
  uint32_t key2 = 0;
  for (int sq = 0; sq < 0x80; sq++) {
    if (sq & MCUMAX_BOARD_MASK)
      continue;
    uint8_t piece = mcumax.board[sq];
    if (piece) {
      key += HashScramble(sq, piece);
      key2 += HashScramble(sq + 8, piece);
    }
  }
  /* Incorporate side to move so hashes are unique per side. */
  key += HashScramble(0x7f, mcumax.current_side);
  key2 += HashScramble(0x7e, mcumax.current_side);

  mcumax.hash_key = key;
  mcumax.hash_key2 = key2;
}

static bool mcumax_is_repetition(void) {
  if (mcumax.history_size < 3)
    return false;
  int limit = (int)mcumax.history_size - 1 - (int)mcumax.halfmove_clock;
  if (limit < 0)
    limit = 0;
  for (int i = (int)mcumax.history_size - 3; i >= limit; i -= 2) {
    if (mcumax.history_keys[i] == mcumax.hash_key) {
      return true;
    }
  }
  return false;
}

struct HashEntry {
  uint32_t key2;
  int32_t score;
  uint8_t square_from;
  uint8_t square_to;
  uint8_t depth;
};

static struct HashEntry mcumax_hash_table[MCUMAX_NUM_CORES][MCUMAX_HASH_TABLE_SIZE];
#define mcumax_current_hash (mcumax_hash_table[(uint32_t)xPortGetCoreID() & 1])

#endif

/* --------------------------------------------------------------------- */
/* Static LCG random generator (not disturbed by app-layer srand calls). */

static uint32_t mcumax_rng_state_instances[MCUMAX_NUM_CORES] = {12345, 67890};
#define mcumax_rng_state (mcumax_rng_state_instances[(uint32_t)xPortGetCoreID() & 1])

static uint32_t mcumax_rand(void) {
  /* Mix in hardware noise on every call. */
  mcumax_rng_state ^= esp_random();

  /* LCG step to smooth the sequence and remove modulo bias. */
  mcumax_rng_state = mcumax_rng_state * 1103515245 + 12345;
  return (uint32_t)(mcumax_rng_state / 65536) % 32768;
}

/* Contempt Factor (Draw avoidance) */
static int32_t mcumax_contempt_instances[MCUMAX_NUM_CORES] = {100, 100};
#define mcumax_contempt (mcumax_contempt_instances[(uint32_t)xPortGetCoreID() & 1])

/* Root move evaluations tracking for probabilistic move selection */
typedef struct {
  mcumax_move legal_moves[100];
  int32_t scores[100];
  float percentages[100];
  uint32_t count;
  bool active;
} mcumax_root_eval_t;

static mcumax_root_eval_t mcumax_root_eval_instances[MCUMAX_NUM_CORES];
#define mcumax_root_eval (mcumax_root_eval_instances[(uint32_t)xPortGetCoreID() & 1])

static bool mcumax_probabilistic_selection_enabled_instances[MCUMAX_NUM_CORES] = {true, true};
#define mcumax_probabilistic_selection_enabled (mcumax_probabilistic_selection_enabled_instances[(uint32_t)xPortGetCoreID() & 1])

void mcumax_set_probabilistic_selection(bool enabled) {
  mcumax_probabilistic_selection_enabled = enabled;
}

bool mcumax_get_probabilistic_selection(void) {
  return mcumax_probabilistic_selection_enabled;
}

/* --------------------------------------------------------------------- */
/* Opening book support */

static const mcumax_opening_entry_t *opening_book_instances[MCUMAX_NUM_CORES] = {NULL, NULL};
#define opening_book (opening_book_instances[(uint32_t)xPortGetCoreID() & 1])

static uint32_t opening_book_entries_instances[MCUMAX_NUM_CORES] = {0, 0};
#define opening_book_entries (opening_book_entries_instances[(uint32_t)xPortGetCoreID() & 1])

/* Convert a 64-square index to the engine's 128-square layout. */
static inline uint8_t to_128_square(uint8_t sq64) {
  return (sq64 / 8) * 16 + (sq64 % 8);
}

/* --------------------------------------------------------------------- */
/* MD5 implementation (RFC 1321) */

typedef struct {
  uint32_t state[4];
  uint32_t count[2];
  uint8_t buffer[64];
} md5_context;

#define MD5_F(x, y, z) ((x & y) | (~x & z))
#define MD5_G(x, y, z) ((x & z) | (y & ~z))
#define MD5_H(x, y, z) (x ^ y ^ z)
#define MD5_I(x, y, z) (y ^ (x | ~z))

#define MD5_ROTLEFT(a, n) (((a) << (n)) | ((a) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac)                                           \
  {                                                                            \
    a += MD5_F(b, c, d) + x + ac;                                              \
    a = MD5_ROTLEFT(a, s);                                                     \
    a += b;                                                                    \
  }
#define MD5_GG(a, b, c, d, x, s, ac)                                           \
  {                                                                            \
    a += MD5_G(b, c, d) + x + ac;                                              \
    a = MD5_ROTLEFT(a, s);                                                     \
    a += b;                                                                    \
  }
#define MD5_HH(a, b, c, d, x, s, ac)                                           \
  {                                                                            \
    a += MD5_H(b, c, d) + x + ac;                                              \
    a = MD5_ROTLEFT(a, s);                                                     \
    a += b;                                                                    \
  }
#define MD5_II(a, b, c, d, x, s, ac)                                           \
  {                                                                            \
    a += MD5_I(b, c, d) + x + ac;                                              \
    a = MD5_ROTLEFT(a, s);                                                     \
    a += b;                                                                    \
  }

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

static void md5_init(md5_context *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xefcdab89;
  ctx->state[2] = 0x98badcfe;
  ctx->state[3] = 0x10325476;
  ctx->count[0] = ctx->count[1] = 0;
}

static void md5_transform(uint32_t *state, const uint8_t *block) {
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t x[16];
  int i;

  for (i = 0; i < 16; i++) {
    x[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
           ((uint32_t)block[i * 4 + 2] << 16) |
           ((uint32_t)block[i * 4 + 3] << 24);
  }

  /* Round 1 */
  MD5_FF(a, b, c, d, x[0], 7, md5_k[0]);
  MD5_FF(d, a, b, c, x[1], 12, md5_k[1]);
  MD5_FF(c, d, a, b, x[2], 17, md5_k[2]);
  MD5_FF(b, c, d, a, x[3], 22, md5_k[3]);
  MD5_FF(a, b, c, d, x[4], 7, md5_k[4]);
  MD5_FF(d, a, b, c, x[5], 12, md5_k[5]);
  MD5_FF(c, d, a, b, x[6], 17, md5_k[6]);
  MD5_FF(b, c, d, a, x[7], 22, md5_k[7]);
  MD5_FF(a, b, c, d, x[8], 7, md5_k[8]);
  MD5_FF(d, a, b, c, x[9], 12, md5_k[9]);
  MD5_FF(c, d, a, b, x[10], 17, md5_k[10]);
  MD5_FF(b, c, d, a, x[11], 22, md5_k[11]);
  MD5_FF(a, b, c, d, x[12], 7, md5_k[12]);
  MD5_FF(d, a, b, c, x[13], 12, md5_k[13]);
  MD5_FF(c, d, a, b, x[14], 17, md5_k[14]);
  MD5_FF(b, c, d, a, x[15], 22, md5_k[15]);

  /* Round 2 */
  MD5_GG(a, b, c, d, x[1], 5, md5_k[16]);
  MD5_GG(d, a, b, c, x[6], 9, md5_k[17]);
  MD5_GG(c, d, a, b, x[11], 14, md5_k[18]);
  MD5_GG(b, c, d, a, x[0], 20, md5_k[19]);
  MD5_GG(a, b, c, d, x[5], 5, md5_k[20]);
  MD5_GG(d, a, b, c, x[10], 9, md5_k[21]);
  MD5_GG(c, d, a, b, x[15], 14, md5_k[22]);
  MD5_GG(b, c, d, a, x[4], 20, md5_k[23]);
  MD5_GG(a, b, c, d, x[9], 5, md5_k[24]);
  MD5_GG(d, a, b, c, x[14], 9, md5_k[25]);
  MD5_GG(c, d, a, b, x[3], 14, md5_k[26]);
  MD5_GG(b, c, d, a, x[8], 20, md5_k[27]);
  MD5_GG(a, b, c, d, x[13], 5, md5_k[28]);
  MD5_GG(d, a, b, c, x[2], 9, md5_k[29]);
  MD5_GG(c, d, a, b, x[7], 14, md5_k[30]);
  MD5_GG(b, c, d, a, x[12], 20, md5_k[31]);

  /* Round 3 */
  MD5_HH(a, b, c, d, x[5], 4, md5_k[32]);
  MD5_HH(d, a, b, c, x[8], 11, md5_k[33]);
  MD5_HH(c, d, a, b, x[11], 16, md5_k[34]);
  MD5_HH(b, c, d, a, x[14], 23, md5_k[35]);
  MD5_HH(a, b, c, d, x[1], 4, md5_k[36]);
  MD5_HH(d, a, b, c, x[4], 11, md5_k[37]);
  MD5_HH(c, d, a, b, x[7], 16, md5_k[38]);
  MD5_HH(b, c, d, a, x[10], 23, md5_k[39]);
  MD5_HH(a, b, c, d, x[13], 4, md5_k[40]);
  MD5_HH(d, a, b, c, x[0], 11, md5_k[41]);
  MD5_HH(c, d, a, b, x[3], 16, md5_k[42]);
  MD5_HH(b, c, d, a, x[6], 23, md5_k[43]);
  MD5_HH(a, b, c, d, x[9], 4, md5_k[44]);
  MD5_HH(d, a, b, c, x[12], 11, md5_k[45]);
  MD5_HH(c, d, a, b, x[15], 16, md5_k[46]);
  MD5_HH(b, c, d, a, x[2], 23, md5_k[47]);

  /* Round 4 */
  MD5_II(a, b, c, d, x[0], 6, md5_k[48]);
  MD5_II(d, a, b, c, x[7], 10, md5_k[49]);
  MD5_II(c, d, a, b, x[14], 15, md5_k[50]);
  MD5_II(b, c, d, a, x[5], 21, md5_k[51]);
  MD5_II(a, b, c, d, x[12], 6, md5_k[52]);
  MD5_II(d, a, b, c, x[3], 10, md5_k[53]);
  MD5_II(c, d, a, b, x[10], 15, md5_k[54]);
  MD5_II(b, c, d, a, x[1], 21, md5_k[55]);
  MD5_II(a, b, c, d, x[8], 6, md5_k[56]);
  MD5_II(d, a, b, c, x[15], 10, md5_k[57]);
  MD5_II(c, d, a, b, x[6], 15, md5_k[58]);
  MD5_II(b, c, d, a, x[13], 21, md5_k[59]);
  MD5_II(a, b, c, d, x[4], 6, md5_k[60]);
  MD5_II(d, a, b, c, x[11], 10, md5_k[61]);
  MD5_II(c, d, a, b, x[2], 15, md5_k[62]);
  MD5_II(b, c, d, a, x[9], 21, md5_k[63]);

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

static void md5_update(md5_context *ctx, const uint8_t *data, size_t len) {
  size_t i;
  uint32_t index = (ctx->count[0] >> 3) & 0x3F;
  if ((ctx->count[0] += (uint32_t)(len << 3)) < (len << 3))
    ctx->count[1]++;
  ctx->count[1] += (uint32_t)(len >> 29);

  size_t part = 64 - index;
  if (len >= part) {
    memcpy(ctx->buffer + index, data, part);
    md5_transform(ctx->state, ctx->buffer);
    for (i = part; i + 63 < len; i += 64)
      md5_transform(ctx->state, data + i);
    index = 0;
  } else {
    i = 0;
  }
  memcpy(ctx->buffer + index, data + i, len - i);
}

static void md5_final(md5_context *ctx, uint8_t *digest) {
  uint8_t bits[8];
  uint32_t index = (ctx->count[0] >> 3) & 0x3F;
  uint32_t pad_len;

  bits[0] = ctx->count[0] & 0xFF;
  bits[1] = (ctx->count[0] >> 8) & 0xFF;
  bits[2] = (ctx->count[0] >> 16) & 0xFF;
  bits[3] = (ctx->count[0] >> 24) & 0xFF;
  bits[4] = ctx->count[1] & 0xFF;
  bits[5] = (ctx->count[1] >> 8) & 0xFF;
  bits[6] = (ctx->count[1] >> 16) & 0xFF;
  bits[7] = (ctx->count[1] >> 24) & 0xFF;

  pad_len = (index < 56) ? (56 - index) : (120 - index);
  uint8_t pad[64];
  pad[0] = 0x80;
  memset(pad + 1, 0, pad_len - 1);
  md5_update(ctx, pad, pad_len);
  md5_update(ctx, bits, 8);

  for (int i = 0; i < 4; i++) {
    digest[i * 4] = ctx->state[i] & 0xFF;
    digest[i * 4 + 1] = (ctx->state[i] >> 8) & 0xFF;
    digest[i * 4 + 2] = (ctx->state[i] >> 16) & 0xFF;
    digest[i * 4 + 3] = (ctx->state[i] >> 24) & 0xFF;
  }
}

/* --------------------------------------------------------------------- */
/* Helper functions for draw detection, opening book and check queries. */

static mcumax_square mcumax_find_king_internal(uint8_t side) {
  for (int sq = 0; sq < 0x80; sq++) {
    if (sq & MCUMAX_BOARD_MASK)
      continue;
    uint8_t piece = mcumax.board[sq];
    if ((piece & 0x18) == side && (piece & 0x7) == MCUMAX_KING)
      return sq;
  }
  return MCUMAX_SQUARE_INVALID;
}

/* Stable (snapshot) variant used by the public query API. */
static mcumax_square mcumax_find_king_stable(uint8_t side) {
  for (int sq = 0; sq < 0x80; sq++) {
    if (sq & MCUMAX_BOARD_MASK)
      continue;
    uint8_t piece = mcumax.stable_board[sq];
    if ((piece & 0x18) == side && (piece & 0x7) == MCUMAX_KING)
      return sq;
  }
  return MCUMAX_SQUARE_INVALID;
}

static bool mcumax_is_square_attacked_internal(mcumax_square sq,
                                                uint8_t attacker_side) {
  /* Scan every piece of attacker_side and see if it can reach `sq`. */
  for (int from = 0; from < 0x80; from++) {
    if (from & MCUMAX_BOARD_MASK)
      continue;
    uint8_t piece = mcumax.board[from];
    if ((piece & 0x18) != attacker_side)
      continue;
    uint8_t type = piece & 0x7;
    int8_t vector_idx = mcumax_step_vectors_indices[type];
    int8_t step;
    while ((step = ((type > 2) && (step < 0))
                       ? -step
                       : -mcumax_step_vectors[++vector_idx])) {
      int to = from;
      do {
        to += step;
        if (to & MCUMAX_BOARD_MASK)
          break;
        if (to == sq)
          return true;
        if (mcumax.board[to])
          break; /* blocked */
      } while (type >= 5); /* sliding pieces only */
    }
  }
  return false;
}

/* Stable variant that uses the snapshot board. This is the public query
 * used by main.cpp's UI (king-in-check highlight, castling validity,
 * move-legality test) so it must NOT consult the working board that
 * may be mid-search. */
static bool mcumax_is_square_attacked_stable(mcumax_square sq,
                                              uint8_t attacker_side) {
  /* Knights */
  int knight_offsets[8] = {-33, -31, -18, -14, 14, 18, 31, 33};
  for (int i = 0; i < 8; i++) {
    int s = sq + knight_offsets[i];
    if (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = mcumax.stable_board[s];
      if ((p & 0x18) == attacker_side && (p & 0x07) == MCUMAX_KNIGHT)
        return true;
    }
  }
  /* Sliding orthogonal: rook, queen */
  int rook_offsets[4] = {-16, -1, 1, 16};
  for (int i = 0; i < 4; i++) {
    int s = sq + rook_offsets[i];
    while (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = mcumax.stable_board[s];
      if (p) {
        if ((p & 0x18) == attacker_side) {
          int type = p & 0x07;
          if (type == MCUMAX_ROOK || type == MCUMAX_QUEEN)
            return true;
        }
        break;
      }
      s += rook_offsets[i];
    }
  }
  /* Sliding diagonal: bishop, queen */
  int bishop_offsets[4] = {-17, -15, 15, 17};
  for (int i = 0; i < 4; i++) {
    int s = sq + bishop_offsets[i];
    while (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = mcumax.stable_board[s];
      if (p) {
        if ((p & 0x18) == attacker_side) {
          int type = p & 0x07;
          if (type == MCUMAX_BISHOP || type == MCUMAX_QUEEN)
            return true;
        }
        break;
      }
      s += bishop_offsets[i];
    }
  }
  /* Pawns. White pawns (MCUMAX_PAWN_UPSTREAM, side 0x08) move upwards
   * (decreasing rank), so they attack squares at sq-15 / sq-17. Black
   * pawns (MCUMAX_PAWN_DOWNSTREAM, side 0x10) move downwards. */
  int pawn_offsets[4] = {-17, -15, 15, 17};
  for (int i = 0; i < 4; i++) {
    int s = sq + pawn_offsets[i];
    if (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = mcumax.stable_board[s];
      if ((p & 0x18) == attacker_side) {
        int type = p & 0x07;
        if (type == MCUMAX_PAWN_UPSTREAM && (sq == s - 15 || sq == s - 17))
          return true;
        if (type == MCUMAX_PAWN_DOWNSTREAM && (sq == s + 15 || sq == s + 17))
          return true;
      }
    }
  }
  /* King adjacency */
  int king_offsets[8] = {-17, -16, -15, -1, 1, 15, 16, 17};
  for (int i = 0; i < 8; i++) {
    int s = sq + king_offsets[i];
    if (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = mcumax.stable_board[s];
      if ((p & 0x18) == attacker_side && (p & 0x07) == MCUMAX_KING)
        return true;
    }
  }
  return false;
}

static bool mcumax_is_in_check_internal(uint8_t side) {
  mcumax_square king = mcumax_find_king_internal(side);
  if (king == MCUMAX_SQUARE_INVALID)
    return false;
  uint8_t opponent = side ^ 0x18;
  return mcumax_is_square_attacked_internal(king, opponent);
}

static int mcumax_insufficient_material_board(const uint8_t *board) {
  int white_pawns = 0, white_knights = 0, white_bishops = 0, white_rooks = 0,
      white_queens = 0;
  int black_pawns = 0, black_knights = 0, black_bishops = 0, black_rooks = 0,
      black_queens = 0;
  int white_bishop_color = -1, black_bishop_color = -1;

  for (int sq = 0; sq < 0x80; sq++) {
    if (sq & MCUMAX_BOARD_MASK)
      continue;
    uint8_t piece = board[sq];
    if (!piece)
      continue;
    int color = piece & 0x18;
    int type = piece & 0x7;
    if (type == MCUMAX_ROOK || type == MCUMAX_QUEEN ||
        type == MCUMAX_PAWN_UPSTREAM || type == MCUMAX_PAWN_DOWNSTREAM)
      return 0;
    int parity = ((sq >> 4) + (sq & 7)) & 1;

    if (color == MCUMAX_BOARD_WHITE) {
      switch (type) {
      case MCUMAX_PAWN_UPSTREAM:
      case MCUMAX_PAWN_DOWNSTREAM:
        white_pawns++;
        break;
      case MCUMAX_KNIGHT:
        white_knights++;
        break;
      case MCUMAX_BISHOP:
        white_bishops++;
        if (white_bishop_color == -1)
          white_bishop_color = parity;
        else if (white_bishop_color != parity)
          white_bishop_color = -2;
        break;
      case MCUMAX_ROOK:
        white_rooks++;
        break;
      case MCUMAX_QUEEN:
        white_queens++;
        break;
      default:
        break;
      }
    } else if (color == MCUMAX_BOARD_BLACK) {
      switch (type) {
      case MCUMAX_PAWN_UPSTREAM:
      case MCUMAX_PAWN_DOWNSTREAM:
        black_pawns++;
        break;
      case MCUMAX_KNIGHT:
        black_knights++;
        break;
      case MCUMAX_BISHOP:
        black_bishops++;
        if (black_bishop_color == -1)
          black_bishop_color = parity;
        else if (black_bishop_color != parity)
          black_bishop_color = -2;
        break;
      case MCUMAX_ROOK:
        black_rooks++;
        break;
      case MCUMAX_QUEEN:
        black_queens++;
        break;
      default:
        break;
      }
    }
  }

  if (white_pawns || black_pawns || white_rooks || black_rooks ||
      white_queens || black_queens)
    return 0;

  if ((white_knights + white_bishops) >= 2 ||
      (black_knights + black_bishops) >= 2)
    return 0;

  if (white_bishops == 1 && black_bishops == 1 && white_knights == 0 &&
      black_knights == 0 && white_bishop_color != -2 &&
      black_bishop_color != -2 && white_bishop_color == black_bishop_color)
    return 1;

  if ((white_knights + white_bishops + black_knights + black_bishops) == 0)
    return 2;

  if ((white_knights + white_bishops + black_knights + black_bishops) == 1)
    return 2;

  return 0;
}

/* --------------------------------------------------------------------- */
/* Board hash for opening-book lookup and threefold-repetition tracking. */

static uint32_t compute_board_hash_internal(const uint8_t *board) {
  char fen[128];
  int index = 0;
  for (int rank = 0; rank < 8; rank++) {
    int empty = 0;
    for (int file = 0; file < 8; file++) {
      int sq = rank * 16 + file;
      uint8_t piece = board[sq];
      if (piece == 0) {
        empty++;
      } else {
        if (empty) {
          fen[index++] = '0' + empty;
          empty = 0;
        }
        char c;
        uint8_t type = piece & 0x7;
        uint8_t color = piece & 0x18;
        if (color == MCUMAX_BOARD_WHITE) {
          if (type == MCUMAX_PAWN_UPSTREAM)
            c = 'P';
          else if (type == MCUMAX_KNIGHT)
            c = 'N';
          else if (type == MCUMAX_BISHOP)
            c = 'B';
          else if (type == MCUMAX_ROOK)
            c = 'R';
          else if (type == MCUMAX_QUEEN)
            c = 'Q';
          else if (type == MCUMAX_KING)
            c = 'K';
          else
            c = '?';
        } else {
          if (type == MCUMAX_PAWN_DOWNSTREAM)
            c = 'p';
          else if (type == MCUMAX_KNIGHT)
            c = 'n';
          else if (type == MCUMAX_BISHOP)
            c = 'b';
          else if (type == MCUMAX_ROOK)
            c = 'r';
          else if (type == MCUMAX_QUEEN)
            c = 'q';
          else if (type == MCUMAX_KING)
            c = 'k';
          else
            c = '?';
        }
        fen[index++] = c;
      }
    }
    if (empty) {
      fen[index++] = '0' + empty;
    }
    if (rank < 7)
      fen[index++] = '/';
  }
  fen[index] = '\0';

  uint8_t digest[16];
  md5_context ctx;
  md5_init(&ctx);
  md5_update(&ctx, (const uint8_t *)fen, strlen(fen));
  md5_final(&ctx, digest);

  return (digest[0] << 24) | (digest[1] << 16) | (digest[2] << 8) | digest[3];
}

/* Opening-book lookup: binary search + roulette-wheel selection. */
mcumax_move mcumax_lookup_opening_move(void) {
  if (!opening_book || opening_book_entries == 0)
    return MCUMAX_MOVE_INVALID;

  uint32_t hash = compute_board_hash_internal(mcumax.board);
  mcumax_move best_move = MCUMAX_MOVE_INVALID;

  int32_t low = 0;
  int32_t high = opening_book_entries - 1;
  int32_t match_idx = -1;

  while (low <= high) {
    int32_t mid = low + (high - low) / 2;
    if (opening_book[mid].hash == hash) {
      match_idx = mid;
      break;
    } else if (opening_book[mid].hash < hash) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  if (match_idx == -1) {
    return MCUMAX_MOVE_INVALID;
  }

  int32_t start = match_idx;
  while (start > 0 && opening_book[start - 1].hash == hash) {
    start--;
  }
  int32_t end = match_idx;
  while (end < (int32_t)opening_book_entries - 1 &&
         opening_book[end + 1].hash == hash) {
    end++;
  }

  uint32_t total_weight = 0;
  for (int32_t i = start; i <= end; i++) {
    uint8_t from = to_128_square(opening_book[i].from);
    uint8_t to = to_128_square(opening_book[i].to);

    uint8_t piece = mcumax.board[from];
    if ((piece & 0x18) == mcumax.current_side) {
      uint8_t dest_piece = mcumax.board[to];
      if ((dest_piece & 0x18) != mcumax.current_side) {
        total_weight += opening_book[i].weight;
      }
    }
  }

  if (total_weight > 0) {
    uint32_t r = esp_random() % total_weight;
    uint32_t cumulative_weight = 0;

    for (int32_t i = start; i <= end; i++) {
      uint8_t from = to_128_square(opening_book[i].from);
      uint8_t to = to_128_square(opening_book[i].to);

      uint8_t piece = mcumax.board[from];
      if ((piece & 0x18) == mcumax.current_side) {
        uint8_t dest_piece = mcumax.board[to];
        if ((dest_piece & 0x18) != mcumax.current_side) {
          cumulative_weight += opening_book[i].weight;
          if (r < cumulative_weight) {
            best_move.from = from;
            best_move.to = to;
            best_move.promotion_piece = 0;
            return best_move;
          }
        }
      }
    }
  }

  return MCUMAX_MOVE_INVALID;
}

bool mcumax_is_opening_move(mcumax_move move) {
  if (!opening_book || opening_book_entries == 0)
    return false;

  uint32_t hash = compute_board_hash_internal(mcumax.board);
  int32_t low = 0;
  int32_t high = opening_book_entries - 1;
  int32_t match_idx = -1;

  while (low <= high) {
    int32_t mid = low + (high - low) / 2;
    if (opening_book[mid].hash == hash) {
      match_idx = mid;
      break;
    } else if (opening_book[mid].hash < hash) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  if (match_idx == -1)
    return false;

  int32_t start = match_idx;
  while (start > 0 && opening_book[start - 1].hash == hash) {
    start--;
  }
  int32_t end = match_idx;
  while (end < (int32_t)opening_book_entries - 1 &&
         opening_book[end + 1].hash == hash) {
    end++;
  }

  for (int32_t i = start; i <= end; i++) {
    uint8_t from = to_128_square(opening_book[i].from);
    uint8_t to = to_128_square(opening_book[i].to);
    if (from == move.from && to == move.to) {
      return true;
    }
  }

  return false;
}

static uint8_t mcumax_search_root_side_instances[MCUMAX_NUM_CORES] = {0x08, 0x08};
#define mcumax_search_root_side (mcumax_search_root_side_instances[(uint32_t)xPortGetCoreID() & 1])

static inline int32_t mcumax_draw_score(int32_t score) {
  if (mcumax.current_side == mcumax_search_root_side) {
    return MCUMAX_DRAW_SCORE - mcumax_contempt;
  } else {
    return MCUMAX_DRAW_SCORE + mcumax_contempt;
  }
}

/* --------------------------------------------------------------------- */
/* Recursive minimax search (preserved from upstream micro-Max 4.8).    */

static int32_t mcumax_search(int32_t alpha, int32_t beta, int32_t score,
                             uint8_t en_passant_square, uint8_t depth,
                             enum mcumax_mode mode) {
  if (mcumax.stop_search)
    return score;

  if (mcumax.node_max > 0 && mcumax.node_count >= mcumax.node_max) {
    mcumax.stop_search = true;
    return score;
  }

  if (mcumax.user_callback)
    mcumax.user_callback(mcumax.user_data);

  if (mode == MCUMAX_INTERNAL_NODE) {
    if (mcumax.halfmove_clock >= 100 || mcumax_is_repetition() ||
        mcumax_insufficient_material_board(mcumax.board)) {
      return mcumax_draw_score(score);
    }
  }

  uint8_t iter_depth;
  int32_t iter_score;
  uint8_t iter_square_from;
  uint8_t iter_square_to;

#ifdef MCUMAX_HASHING_ENABLED
  int32_t hash_key;
  int32_t hash_key2;
#endif

  uint8_t square_start;

  uint8_t square_from;
  uint8_t square_to;

  uint8_t replay_move;
  int32_t null_move_score;

  uint8_t scan_piece;
  uint8_t scan_piece_type;

  int8_t step_vector;
  int8_t step_vector_index;

  uint8_t castling_skip_square;
  uint8_t castling_rook_square;

  uint8_t capture_square;
  uint8_t capture_piece;
  int32_t capture_piece_value;

  uint8_t step_depth;
  int32_t step_alpha;
  int32_t step_score;
  int32_t step_score_new;

  alpha -= alpha < score;
  beta -= beta <= score;

#ifdef MCUMAX_HASHING_ENABLED
  struct HashEntry *hash_entry =
      mcumax_current_hash +
      ((mcumax.hash_key + mcumax.current_side * en_passant_square) &
       (MCUMAX_HASH_TABLE_SIZE - 1));

  iter_depth = hash_entry->depth;
  iter_score = hash_entry->score;
  iter_square_from = hash_entry->square_from;
  iter_square_to = hash_entry->square_to;

  if ((hash_entry->key2 != mcumax.hash_key2) ||
      (mode != MCUMAX_INTERNAL_NODE) ||
      !(((iter_score <= alpha) || (iter_square_from & 0x8)) &&
        ((iter_score >= beta) || (iter_square_from & MCUMAX_SQUARE_INVALID)))) {
    iter_depth = iter_square_to = 0;
  }

  iter_square_from &= ~MCUMAX_BOARD_MASK;

  hash_key = mcumax.hash_key;
  hash_key2 = mcumax.hash_key2;
#else
  iter_depth = iter_score = iter_square_from = iter_square_to = 0;
#endif

  while ((iter_depth++ < depth) || (iter_depth < 3) ||
         ((mode != MCUMAX_INTERNAL_NODE) &&
          (mcumax.square_from == MCUMAX_SQUARE_INVALID) &&
          ((((mcumax.node_max == 0) || (mcumax.node_count < mcumax.node_max)) &&
            (iter_depth <= mcumax.depth_max)) ||
           (mcumax.square_from = iter_square_from,
            mcumax.square_to = iter_square_to & ~MCUMAX_BOARD_MASK,
            iter_depth = 3)))) {
    if (mcumax.stop_search)
      break;

    square_from = square_start =
        (mode != MCUMAX_SEARCH_VALID_MOVES) ? iter_square_from : 0;

    replay_move = iter_square_to & MCUMAX_SQUARE_INVALID;

    mcumax.current_side ^= 0x18;

    null_move_score =
        (iter_depth > 2) && (beta != -MCUMAX_SCORE_MAX)
            ? mcumax_search(-beta, 1 - beta, -score, MCUMAX_SQUARE_INVALID,
                            iter_depth - 3, MCUMAX_INTERNAL_NODE)
            : MCUMAX_SCORE_MAX;

    mcumax.current_side ^= 0x18;

    iter_score = (-null_move_score < beta) || (mcumax.non_pawn_material > 35)
                     ? (iter_depth - 2) ? -MCUMAX_SCORE_MAX : score
                     : -null_move_score;

    mcumax.node_count++;

    do {
      scan_piece = mcumax.board[square_from];

      if (scan_piece & mcumax.current_side) {
        step_vector = scan_piece_type = (scan_piece & 0b111);
        step_vector_index = mcumax_step_vectors_indices[scan_piece_type];

        while (
            (step_vector = ((scan_piece_type > 2) && (step_vector < 0))
                               ? -step_vector
                               : -mcumax_step_vectors[++step_vector_index])) {
        replay:
          square_to = square_from;
          castling_skip_square = castling_rook_square = MCUMAX_SQUARE_INVALID;

          do {
            capture_square = square_to = replay_move
                                             ? (iter_square_to ^ replay_move)
                                             : (square_to + step_vector);

            if (square_to & MCUMAX_BOARD_MASK)
              break;

            if ((en_passant_square != MCUMAX_SQUARE_INVALID) &&
                mcumax.board[en_passant_square] &&
                ((square_to - en_passant_square) < 2) &&
                ((en_passant_square - square_to) < 2))
              iter_score = MCUMAX_SCORE_MAX;

            if ((scan_piece_type < 3) && (square_to == en_passant_square))
              capture_square ^= 16;

            capture_piece = mcumax.board[capture_square];

            if ((capture_piece & mcumax.current_side) ||
                ((scan_piece_type < 3) &&
                 !((square_to - square_from) & 0b111) - !capture_piece))
              break;

            capture_piece_value =
                37 * mcumax_capture_values[capture_piece & 0b111] +
                (capture_piece & 0xc0);

            if (capture_piece_value < 0) {
              iter_score = MCUMAX_SCORE_MAX;
              iter_depth = MCUMAX_DEPTH_MAX - 1;
            }

            if ((iter_score >= beta) && (iter_depth > 1))
              goto cutoff;

            step_score = (iter_depth != 1)
                             ? score
                             : capture_piece_value - scan_piece_type;

            if ((iter_depth - !capture_piece) > 1) {
              step_score = (scan_piece_type < 6)
                               ? mcumax.board[square_from + 0x8] -
                                     mcumax.board[square_to + 0x8]
                               : 0;

              uint32_t old_history_size = mcumax.history_size;
              int32_t old_halfmove_clock = mcumax.halfmove_clock;

              mcumax.board[castling_rook_square] =
                  mcumax.board[capture_square] = mcumax.board[square_from] = 0;
              mcumax.board[square_to] = scan_piece | MCUMAX_PIECE_MOVED;

              if (!(castling_rook_square & MCUMAX_BOARD_MASK)) {
                mcumax.board[castling_skip_square] = mcumax.current_side + 6;
                step_score += 50;
              }

              step_score -=
                  ((scan_piece_type != 4) || (mcumax.non_pawn_material > 30))
                      ? 0
                      : 20;

              if (scan_piece_type < 3) {
                step_score -=
                    9 * ((((square_from - 2) & MCUMAX_BOARD_MASK) ||
                          mcumax.board[square_from - 2] - scan_piece) +
                         (((square_from + 2) & MCUMAX_BOARD_MASK) ||
                          mcumax.board[square_from + 2] - scan_piece) -
                         1 +
                         (mcumax.board[square_from ^ 0x10] ==
                          (mcumax.current_side + 36))) -
                    (mcumax.non_pawn_material >> 2);

                capture_piece_value += step_alpha =
                    (square_to + step_vector + 1) & MCUMAX_SQUARE_INVALID
                        ? (647 - scan_piece_type)
                        : 2 * (scan_piece & (square_to + 0x10) & 0x20);

                mcumax.board[square_to] += step_alpha;
              }

#ifdef MCUMAX_HASHING_ENABLED
              /* Recalculate hash dynamically to be 100% correct, including
               * castling, EP and promotion. Toggle the side to move
               * temporarily because the child node will evaluate with the
               * toggled side. */
              mcumax.current_side ^= 0x18;
              mcumax_recalculate_hash();
              mcumax.current_side ^= 0x18;
#endif

              if (capture_piece || scan_piece_type < 3) {
                mcumax.halfmove_clock = 0;
              } else {
                mcumax.halfmove_clock++;
              }
              if (mcumax.history_size < MCUMAX_HISTORY_SIZE)
                mcumax.history_keys[mcumax.history_size++] = mcumax.hash_key;

              step_score += score + capture_piece_value;
              step_alpha = (mode == MCUMAX_SEARCH_BEST_MOVE &&
                            mcumax.square_from == MCUMAX_SQUARE_INVALID)
                               ? alpha
                               : (iter_score > alpha ? iter_score : alpha);

              step_depth = iter_depth - 1 -
                           ((iter_depth > 5) && (scan_piece_type > 2) &&
                            !capture_piece && !replay_move);

              if (!((mcumax.non_pawn_material > 30) ||
                    (null_move_score - MCUMAX_SCORE_MAX) || (iter_depth < 3) ||
                    (capture_piece && (scan_piece_type != 4))))
                step_depth = iter_depth;

              do {
                mcumax.current_side ^= 0x18;

                step_score_new =
                    ((mode == MCUMAX_SEARCH_VALID_MOVES) || (step_depth > 2) ||
                     (step_score > step_alpha))
                        ? -mcumax_search(-beta, -step_alpha, -step_score,
                                         castling_skip_square, step_depth,
                                         MCUMAX_INTERNAL_NODE)
                        : step_score;

                mcumax.current_side ^= 0x18;
              } while ((step_score_new > alpha) && (++step_depth < iter_depth));

              step_score = step_score_new;

              mcumax.history_size = old_history_size;
              mcumax.halfmove_clock = old_halfmove_clock;

              if ((mode == MCUMAX_PLAY_MOVE) &&
                  (step_score != -MCUMAX_SCORE_MAX) &&
                  (square_from == mcumax.square_from) &&
                  (square_to == mcumax.square_to)) {
                mcumax.score = -score - capture_piece_value;
                mcumax.en_passant_square = castling_skip_square;

#ifdef MCUMAX_HASHING_ENABLED
                hash_entry->depth = MCUMAX_DEPTH_MAX;
                hash_entry->score = 0;
#endif

                mcumax.non_pawn_material += capture_piece_value >> 7;
                mcumax.current_side ^= 0x18;

                if (capture_piece || scan_piece_type < 3) {
                  mcumax.halfmove_clock = 0;
                } else {
                  mcumax.halfmove_clock++;
                }
                if (mcumax.history_size < MCUMAX_HISTORY_SIZE)
                  mcumax.history_keys[mcumax.history_size++] = mcumax.hash_key;

                return beta;
              }

#ifdef MCUMAX_HASHING_ENABLED
              mcumax.hash_key = hash_key;
              mcumax.hash_key2 = hash_key2;
#endif

              mcumax.board[castling_rook_square] = mcumax.current_side + 6;
              mcumax.board[castling_skip_square] = mcumax.board[square_to] = 0;
              mcumax.board[square_from] = scan_piece;
              mcumax.board[capture_square] = capture_piece;

              if ((mode == MCUMAX_SEARCH_BEST_MOVE) &&
                  (step_score != -MCUMAX_SCORE_MAX) &&
                  (square_from == mcumax.square_from) &&
                  (square_to == mcumax.square_to))
                return beta;

              if ((mode == MCUMAX_SEARCH_VALID_MOVES) &&
                  (step_score != -MCUMAX_SCORE_MAX) &&
                  (mcumax.square_from == MCUMAX_SQUARE_INVALID) &&
                  (iter_depth == 3) && !replay_move) {
                mcumax_move move = {square_from, square_to, 0};
                if (mcumax.valid_moves_num < mcumax.valid_moves_buffer_size)
                  mcumax.valid_moves_buffer[mcumax.valid_moves_num] = move;
                mcumax.valid_moves_num++;
              }

              if (mcumax_root_eval.active &&
                  (mode == MCUMAX_SEARCH_BEST_MOVE) &&
                  (mcumax.square_from == MCUMAX_SQUARE_INVALID)) {
                for (uint32_t i = 0; i < mcumax_root_eval.count; i++) {
                  if (mcumax_root_eval.legal_moves[i].from == square_from &&
                      mcumax_root_eval.legal_moves[i].to == square_to) {
                    mcumax_root_eval.scores[i] = step_score;
                    break;
                  }
                }
              }
            }

            if (step_score > iter_score) {
              iter_score = step_score;
              iter_square_from = square_from;
              iter_square_to =
                  square_to | (castling_skip_square & MCUMAX_SQUARE_INVALID);
            }

            if (replay_move) {
              replay_move = 0;
              goto replay;
            }

            if ((square_from + step_vector - square_to) ||
                (scan_piece & MCUMAX_PIECE_MOVED) ||
                ((scan_piece_type > 2) &&
                 (((scan_piece_type != 4) || (step_vector_index != 7) ||
                   (mcumax.board[castling_rook_square =
                                     ((square_from + 3) ^
                                      ((step_vector >> 1) & 0b111))] -
                    mcumax.current_side - 6) ||
                   mcumax.board[castling_rook_square ^ 1] ||
                   mcumax.board[castling_rook_square ^ 2]))))
              capture_piece += (scan_piece_type < 5);
            else
              castling_skip_square = square_to;

          } while (!capture_piece);
        }
      }

    } while ((square_from = ((square_from + 9) & ~MCUMAX_BOARD_MASK)) !=
             square_start);

  cutoff:
    if ((iter_score == -MCUMAX_SCORE_MAX) &&
        (null_move_score != MCUMAX_SCORE_MAX)) {
      if (!mcumax_is_in_check_internal(mcumax.current_side)) {
        iter_score = mcumax_draw_score(score); /* stalemate */
      } else {
        iter_score = -MCUMAX_SCORE_MAX; /* checkmate */
      }
    }

#ifdef MCUMAX_HASHING_ENABLED
    if (hash_entry->depth < MCUMAX_DEPTH_MAX) {
      hash_entry->key2 = mcumax.hash_key2;
      hash_entry->score = iter_score;
      hash_entry->depth = iter_depth;
      hash_entry->square_from = iter_square_from | 8 * (iter_score > alpha) |
                                MCUMAX_SQUARE_INVALID * (iter_score < beta);
      hash_entry->square_to = iter_square_to;
    }
#endif

    if (mode == MCUMAX_SEARCH_BEST_MOVE && mcumax.square_from == MCUMAX_SQUARE_INVALID &&
        iter_square_from != MCUMAX_SQUARE_INVALID && !mcumax.stop_search) {
      mcumax.root_best_from = iter_square_from;
      mcumax.root_best_to = iter_square_to & ~MCUMAX_BOARD_MASK;
    }
  }

  return iter_score += iter_score < score;
}

/* --------------------------------------------------------------------- */
/* Stable-snapshot sync: copy the working board into the stable snapshot
 * and refresh derived state used by the public query API. */

static void mcumax_sync_stable_snapshot(void) {
  memcpy(mcumax.stable_board, mcumax.board, sizeof(mcumax.stable_board));
  mcumax.stable_current_side = mcumax.current_side;
  mcumax.stable_en_passant_square = mcumax.en_passant_square;
  mcumax.stable_halfmove_clock = mcumax.halfmove_clock;
}

/* Castling-rights helpers. The engine internally tracks castling via
 * the MCUMAX_PIECE_MOVED bit on the king and rooks; the 4-bit FEN
 * mask is a derived view that is convenient for the app layer. */
static void mcumax_recompute_castling_rights(void) {
  uint8_t rights = 0;
  /* White king at 0x74 + rook at 0x77 (kingside) */
  if (!(mcumax.board[0x74] & MCUMAX_PIECE_MOVED) &&
      (mcumax.board[0x74] & 0x7) == MCUMAX_KING &&
      (mcumax.board[0x74] & 0x18) == MCUMAX_BOARD_WHITE) {
    if (!(mcumax.board[0x77] & MCUMAX_PIECE_MOVED) &&
        (mcumax.board[0x77] & 0x7) == MCUMAX_ROOK &&
        (mcumax.board[0x77] & 0x18) == MCUMAX_BOARD_WHITE)
      rights |= MCUMAX_CASTLE_WHITE_KINGSIDE;
    if (!(mcumax.board[0x70] & MCUMAX_PIECE_MOVED) &&
        (mcumax.board[0x70] & 0x7) == MCUMAX_ROOK &&
        (mcumax.board[0x70] & 0x18) == MCUMAX_BOARD_WHITE)
      rights |= MCUMAX_CASTLE_WHITE_QUEENSIDE;
  }
  /* Black king at 0x04 + rook at 0x07 (kingside) */
  if (!(mcumax.board[0x04] & MCUMAX_PIECE_MOVED) &&
      (mcumax.board[0x04] & 0x7) == MCUMAX_KING &&
      (mcumax.board[0x04] & 0x18) == MCUMAX_BOARD_BLACK) {
    if (!(mcumax.board[0x07] & MCUMAX_PIECE_MOVED) &&
        (mcumax.board[0x07] & 0x7) == MCUMAX_ROOK &&
        (mcumax.board[0x07] & 0x18) == MCUMAX_BOARD_BLACK)
      rights |= MCUMAX_CASTLE_BLACK_KINGSIDE;
    if (!(mcumax.board[0x00] & MCUMAX_PIECE_MOVED) &&
        (mcumax.board[0x00] & 0x7) == MCUMAX_ROOK &&
        (mcumax.board[0x00] & 0x18) == MCUMAX_BOARD_BLACK)
      rights |= MCUMAX_CASTLE_BLACK_QUEENSIDE;
  }
  mcumax.castling_rights = rights;
}

/* Update castling rights after a move was played on the working board. */
static void mcumax_update_castling_rights_for_move(mcumax_move m) {
  switch (m.from) {
  case 0x74:
    mcumax.castling_rights &=
        ~(MCUMAX_CASTLE_WHITE_KINGSIDE | MCUMAX_CASTLE_WHITE_QUEENSIDE);
    break;
  case 0x77:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_WHITE_KINGSIDE;
    break;
  case 0x70:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_WHITE_QUEENSIDE;
    break;
  case 0x04:
    mcumax.castling_rights &=
        ~(MCUMAX_CASTLE_BLACK_KINGSIDE | MCUMAX_CASTLE_BLACK_QUEENSIDE);
    break;
  case 0x07:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_BLACK_KINGSIDE;
    break;
  case 0x00:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_BLACK_QUEENSIDE;
    break;
  default:
    break;
  }
  /* If a rook was captured on its starting square, that side loses
   * the corresponding castling right too. */
  switch (m.to) {
  case 0x77:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_WHITE_KINGSIDE;
    break;
  case 0x70:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_WHITE_QUEENSIDE;
    break;
  case 0x07:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_BLACK_KINGSIDE;
    break;
  case 0x00:
    mcumax.castling_rights &= ~MCUMAX_CASTLE_BLACK_QUEENSIDE;
    break;
  default:
    break;
  }
}

/* Update the en-passant target square after a move (used by the public
 * play_move path so the snapshot reflects the post-move state). */
static void mcumax_update_en_passant_for_move(mcumax_move m,
                                               uint8_t moving_piece) {
  uint8_t type = moving_piece & 0x7;
  if (type == MCUMAX_PAWN_UPSTREAM || type == MCUMAX_PAWN_DOWNSTREAM) {
    int from_rank = m.from >> 4;
    int to_rank = m.to >> 4;
    int file = m.from & 0x07;
    int diff = (int)to_rank - (int)from_rank;
    if (diff == 2 || diff == -2) {
      int pass_rank = (from_rank + to_rank) >> 1;
      mcumax.en_passant_square = (pass_rank << 4) | file;
      return;
    }
  }
  mcumax.en_passant_square = MCUMAX_SQUARE_INVALID;
}

/* --------------------------------------------------------------------- */
/* Public API */

void mcumax_init() {
  for (uint32_t x = 0; x < 8; x++) {
    mcumax.board[0x10 * 0 + x] = MCUMAX_BOARD_BLACK | mcumax_board_setup[x];
    mcumax.board[0x10 * 1 + x] = MCUMAX_BOARD_BLACK | MCUMAX_PAWN_DOWNSTREAM;
    for (uint32_t y = 2; y < 6; y++)
      mcumax.board[0x10 * y + x] = MCUMAX_EMPTY;
    mcumax.board[0x10 * 6 + x] = MCUMAX_BOARD_WHITE | MCUMAX_PAWN_UPSTREAM;
    mcumax.board[0x10 * 7 + x] = MCUMAX_BOARD_WHITE | mcumax_board_setup[x];

    for (uint32_t y = 0; y < 8; y++)
      mcumax.board[16 * y + x + 8] = (x - 4) * (x - 4) + (y - 4) * (y - 3);
  }
  mcumax.current_side = MCUMAX_BOARD_WHITE;

  mcumax.score = 0;
  mcumax.en_passant_square = MCUMAX_SQUARE_INVALID;
  mcumax.non_pawn_material = 0;

  mcumax.halfmove_clock = 0;
  mcumax.history_size = 0;

  mcumax.castling_rights = MCUMAX_CASTLE_ALL;
  mcumax.last_promotion_piece = 0;

#ifdef MCUMAX_HASHING_ENABLED
  mcumax.hash_key = 0;
  mcumax.hash_key2 = 0;

  memset(mcumax_current_hash, 0, sizeof(mcumax_hash_table[0]));

  static bool scramble_initialized = false;
  if (!scramble_initialized) {
    srand(1);
    for (uint32_t i = 0; i < 1035; i++)
      mcumax_scramble_table[i] =
          ((rand() & 0xff) << 0) | ((rand() & 0xff) << 8) |
          ((rand() & 0xff) << 16) | ((rand() & 0xff) << 24);
    scramble_initialized = true;
  }

  mcumax_recalculate_hash();
  mcumax.history_keys[mcumax.history_size++] = mcumax.hash_key;
#endif

  mcumax_sync_stable_snapshot();
}

void mcumax_set_opening_book(const mcumax_opening_entry_t *book,
                             uint32_t num_entries) {
  opening_book = book;
  opening_book_entries = num_entries;
}

static mcumax_square mcumax_set_piece(mcumax_square square,
                                      mcumax_piece piece) {
  if (square & MCUMAX_BOARD_MASK)
    return square;
  if (piece) {
    uint8_t type = piece & 0x07;
    int rank = square >> 4;
    bool is_home_pawn = (type == MCUMAX_PAWN_UPSTREAM && rank == 6) ||
                        (type == MCUMAX_PAWN_DOWNSTREAM && rank == 1);
    mcumax.board[square] = is_home_pawn ? piece : (piece | MCUMAX_PIECE_MOVED);
  } else {
    mcumax.board[square] = MCUMAX_EMPTY;
  }
  return square + 1;
}

mcumax_piece mcumax_get_piece(mcumax_square square) {
  /* Reads the stable snapshot so queries during search return the
   * pre-search board state. */
  if (square & MCUMAX_BOARD_MASK)
    return MCUMAX_EMPTY;
  return mcumax.stable_board[square] & 0x1F; // Exclude MOVED bit (0x20)
}

void mcumax_set_fen_position(const char *fen_string) {
  mcumax_init();

  uint32_t field_index = 0;
  uint32_t board_index = 0;
  char c;
  while ((c = *fen_string++)) {
    if (c == ' ') {
      if (field_index < 6)
        field_index++;
      continue;
    }

    switch (field_index) {
    case 0:
      if (board_index < 0x80) {
        switch (c) {
        case '8':
        case '7':
        case '6':
        case '5':
        case '4':
        case '3':
        case '2':
        case '1':
          for (int32_t i = 0; i < (c - '0'); i++)
            board_index = mcumax_set_piece(board_index, MCUMAX_EMPTY);
          break;
        case 'P':
          board_index = mcumax_set_piece(board_index, MCUMAX_PAWN_UPSTREAM |
                                                          MCUMAX_BOARD_WHITE);
          break;
        case 'N':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_KNIGHT | MCUMAX_BOARD_WHITE);
          break;
        case 'B':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_BISHOP | MCUMAX_BOARD_WHITE);
          break;
        case 'R':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_ROOK | MCUMAX_BOARD_WHITE);
          break;
        case 'Q':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_QUEEN | MCUMAX_BOARD_WHITE);
          break;
        case 'K':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_KING | MCUMAX_BOARD_WHITE);
          break;
        case 'p':
          board_index = mcumax_set_piece(board_index, MCUMAX_PAWN_DOWNSTREAM |
                                                          MCUMAX_BOARD_BLACK);
          break;
        case 'n':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_KNIGHT | MCUMAX_BOARD_BLACK);
          break;
        case 'b':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_BISHOP | MCUMAX_BOARD_BLACK);
          break;
        case 'r':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_ROOK | MCUMAX_BOARD_BLACK);
          break;
        case 'q':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_QUEEN | MCUMAX_BOARD_BLACK);
          break;
        case 'k':
          board_index =
              mcumax_set_piece(board_index, MCUMAX_KING | MCUMAX_BOARD_BLACK);
          break;
        case '/':
          board_index =
              (board_index < 0x80) ? (board_index & 0xf0) + 0x10 : board_index;
          break;
        }
      }
      break;

    case 1:
      if (c == 'w')
        mcumax.current_side = MCUMAX_BOARD_WHITE;
      else if (c == 'b')
        mcumax.current_side = MCUMAX_BOARD_BLACK;
      break;

    case 2:
      /* The FEN castling flags are applied by clearing the PIECE_MOVED
       * bit on the relevant king/rook squares so the engine recognises
       * castling is still available. */
      switch (c) {
      case 'K':
        mcumax.board[0x74] &= ~MCUMAX_PIECE_MOVED;
        mcumax.board[0x77] &= ~MCUMAX_PIECE_MOVED;
        break;
      case 'Q':
        mcumax.board[0x74] &= ~MCUMAX_PIECE_MOVED;
        mcumax.board[0x70] &= ~MCUMAX_PIECE_MOVED;
        break;
      case 'k':
        mcumax.board[0x04] &= ~MCUMAX_PIECE_MOVED;
        mcumax.board[0x07] &= ~MCUMAX_PIECE_MOVED;
        break;
      case 'q':
        mcumax.board[0x04] &= ~MCUMAX_PIECE_MOVED;
        mcumax.board[0x00] &= ~MCUMAX_PIECE_MOVED;
        break;
      }
      break;

    case 3:
      if (c >= 'a' && c <= 'h') {
        mcumax.en_passant_square &= 0x7f;
        mcumax.en_passant_square |= (c - 'a');
      } else if (c >= '1' && c <= '8') {
        mcumax.en_passant_square &= 0x7f;
        mcumax.en_passant_square |= 16 * ('8' - c);
      }
      break;

    case 4:
      if (c >= '0' && c <= '9') {
        mcumax.halfmove_clock = mcumax.halfmove_clock * 10 + (c - '0');
      }
      break;

    case 5:
      break;
    }
  }

#ifdef MCUMAX_HASHING_ENABLED
  mcumax_recalculate_hash();
  mcumax.history_size = 0;
  mcumax.history_keys[mcumax.history_size++] = mcumax.hash_key;
#endif

  mcumax_recompute_castling_rights();
  mcumax_sync_stable_snapshot();
}

mcumax_piece mcumax_get_current_side(void) {
  return mcumax.stable_current_side;
}

static int32_t mcumax_start_search(enum mcumax_mode mode, mcumax_move move,
                                   uint32_t depth_max, uint32_t node_max) {
  mcumax_search_root_side = mcumax.current_side;

  mcumax.square_from = move.from;
  mcumax.square_to = move.to;
  mcumax.root_best_from = MCUMAX_SQUARE_INVALID;
  mcumax.root_best_to = MCUMAX_SQUARE_INVALID;

  mcumax.node_max = node_max;
  mcumax.node_count = 0;
  mcumax.depth_max = depth_max;

  mcumax.stop_search = false;

  return mcumax_search(-MCUMAX_SCORE_MAX, MCUMAX_SCORE_MAX, mcumax.score,
                       mcumax.en_passant_square, 3, mode);
}

uint32_t mcumax_search_valid_moves(mcumax_move *valid_moves_buffer,
                                   uint32_t valid_moves_buffer_size) {
  mcumax.valid_moves_num = 0;
  mcumax.valid_moves_buffer = valid_moves_buffer;
  mcumax.valid_moves_buffer_size = valid_moves_buffer_size;

  mcumax_start_search(MCUMAX_SEARCH_VALID_MOVES, MCUMAX_MOVE_INVALID, 0, 0);

  /* Restore snapshot state that mcumax_search may have transiently
   * modified (it restores working state itself, but the snapshot is
   * deliberately never touched by the search). */
  return mcumax.valid_moves_num;
}

/* --------------------------------------------------------------------- */
/* Promotion-piece selection.
 *
 * After the main search returns a best move, if that move is a pawn
 * promotion we evaluate all four candidate pieces (Q, R, B, N) with
 * a shallow negamax search and pick the best one. This is integrated
 * into mcumax_search_best_move() so the app layer does not need to
 * handle the engine's promotion choice at all. */

static mcumax_piece mcumax_select_promotion_piece(mcumax_move move,
                                                   uint32_t depth_max) {
  static const uint8_t promo_pieces[4] = {MCUMAX_QUEEN, MCUMAX_ROOK,
                                           MCUMAX_BISHOP, MCUMAX_KNIGHT};

  uint8_t from = move.from;
  uint8_t to = move.to;
  uint8_t moving_piece = mcumax.board[from];
  uint8_t side = moving_piece & 0x18;
  uint8_t captured_piece = mcumax.board[to];

  /* Save working state we are going to perturb. We do NOT save the
   * transposition table: it is 48 KB on ESP32-S3 (4096 entries x 12
   * bytes) and saving it on the stack would crash the device. The
   * comparison searches query different positions (post-promotion)
   * than the main search (pre-promotion), so their TT entries land
   * in different slots and do not corrupt the main search. The
   * shallow (depth 2-3) entries left behind are simply treated as
   * stale cache by the next search and refreshed as needed. */
  uint8_t saved_en_passant = mcumax.en_passant_square;
  int32_t saved_halfmove = mcumax.halfmove_clock;
  uint8_t saved_side = mcumax.current_side;
  uint32_t saved_history_size = mcumax.history_size;
  uint32_t saved_node_count = mcumax.node_count;
#ifdef MCUMAX_HASHING_ENABLED
  uint32_t saved_hash_key = mcumax.hash_key;
  uint32_t saved_hash_key2 = mcumax.hash_key2;
#endif

  /* Use a shallow depth for the comparison so the 4-ply sweep stays
   * fast even on an ESP32-S3. Two plies is enough to detect most
   * tactical reasons to prefer an underpromotion (stalemate traps,
   * knight checks giving fork wins, etc.). */
  uint32_t compare_depth = depth_max > 0 ? depth_max : 2;
  if (compare_depth > 3)
    compare_depth = 3;
  if (compare_depth < 2)
    compare_depth = 2;

  int32_t best_score = -MCUMAX_SCORE_MAX - 1;
  mcumax_piece best_piece = MCUMAX_QUEEN;

  for (int i = 0; i < 4; i++) {
    /* Apply the promotion: pawn at `from` becomes promo_pieces[i] at
     * `to`. Captured piece (if any) is removed. */
    mcumax.board[to] = side | promo_pieces[i] | MCUMAX_PIECE_MOVED;
    mcumax.board[from] = 0;

    /* Switch to opponent: the search evaluates the resulting position
     * from the opponent's perspective; we negate to get our score. */
    mcumax.current_side = side ^ 0x18;
    mcumax.en_passant_square = MCUMAX_SQUARE_INVALID;
    mcumax.halfmove_clock = 0; /* pawn move resets the clock */
    mcumax.stop_search = false;

#ifdef MCUMAX_HASHING_ENABLED
    mcumax_recalculate_hash();
    mcumax.history_size = saved_history_size;
    mcumax.history_keys[mcumax.history_size++] = mcumax.hash_key;
#endif

    mcumax_search_root_side = side; /* our side = root side */

    int32_t opp_score = mcumax_search(-MCUMAX_SCORE_MAX, MCUMAX_SCORE_MAX, 0,
                                       MCUMAX_SQUARE_INVALID, compare_depth,
                                       MCUMAX_INTERNAL_NODE);
    int32_t our_score = -opp_score;

    if (our_score > best_score) {
      best_score = our_score;
      best_piece = promo_pieces[i];
    }

    /* Restore board for next iteration. */
    mcumax.board[to] = captured_piece;
    mcumax.board[from] = moving_piece;
  }

  /* Restore working state. */
  mcumax.current_side = saved_side;
  mcumax.en_passant_square = saved_en_passant;
  mcumax.halfmove_clock = saved_halfmove;
  mcumax.history_size = saved_history_size;
  mcumax.node_count = saved_node_count;
  mcumax.stop_search = false;
#ifdef MCUMAX_HASHING_ENABLED
  mcumax.hash_key = saved_hash_key;
  mcumax.hash_key2 = saved_hash_key2;
#endif

  return best_piece;
}

/* Safety net: the raw negamax search (inherited from micro-Max) relies
 * on the *opponent* finding a king-capturing reply in order to punish
 * a move that leaves (or walks) the mover's own king into check/
 * adjacency. At shallow depth_max/node_max budgets (as used for the
 * low-ELO calibration presets) that reply is sometimes never searched,
 * so the raw search can return a move that is illegal under real chess
 * rules (e.g. moving the king next to the enemy king, or leaving the
 * king exposed to a protected piece). mcumax_is_legal_move() /
 * mcumax_is_castling_move_valid() are cheap, always-correct static
 * checks (no search needed), so we use them here as a hard filter
 * before the engine is allowed to commit to a move. */
static mcumax_move mcumax_pick_safe_fallback_move(void) {
  mcumax_move legal[100];
  uint32_t count = mcumax_get_legal_moves(legal, 100);
  if (count == 0)
    return MCUMAX_MOVE_INVALID;

  /* Prefer the highest-value capture among the legal moves so the
   * fallback still plays reasonably; otherwise just take the first
   * legal move. */
  mcumax_move best = legal[0];
  int32_t best_value = -1;
  for (uint32_t i = 0; i < count; i++) {
    uint8_t captured = mcumax.stable_board[legal[i].to];
    int32_t value = captured ? mcumax_capture_values[captured & 0x7] : 0;
    if (value > best_value) {
      best_value = value;
      best = legal[i];
    }
  }
  return best;
}

mcumax_move mcumax_search_best_move(uint32_t node_max, uint32_t depth_max) {
  mcumax.last_promotion_piece = 0;

  mcumax_move legal_moves[100];
  uint32_t legal_count = mcumax_get_legal_moves(legal_moves, 100);
  if (legal_count == 0)
    return MCUMAX_MOVE_INVALID; /* Checkmate or Stalemate */

  if (opening_book && opening_book_entries > 0) {
    mcumax_move book_move = mcumax_lookup_opening_move();
    if (book_move.from != MCUMAX_SQUARE_INVALID) {
      for (uint32_t i = 0; i < legal_count; i++) {
        if (legal_moves[i].from == book_move.from && legal_moves[i].to == book_move.to) {
          book_move.promotion_piece = 0;
          return book_move;
        }
      }
    }
  }

  /* Initialize root evaluation tracking for all legal moves */
  mcumax_root_eval.count = legal_count;
  for (uint32_t i = 0; i < legal_count; i++) {
    mcumax_root_eval.legal_moves[i] = legal_moves[i];
    mcumax_root_eval.scores[i] = -MCUMAX_SCORE_MAX;
    mcumax_root_eval.percentages[i] = 0.0f;
  }
  mcumax_root_eval.active = true;

  int32_t score = mcumax_start_search(
      MCUMAX_SEARCH_BEST_MOVE, MCUMAX_MOVE_INVALID, depth_max, node_max);
  (void)score;

  mcumax_root_eval.active = false;
  mcumax.last_search_node_count = mcumax.node_count;

  mcumax_move best_move;
  best_move.from = (mcumax.root_best_from != MCUMAX_SQUARE_INVALID)
                       ? mcumax.root_best_from
                       : mcumax.square_from;
  best_move.to = (mcumax.root_best_from != MCUMAX_SQUARE_INVALID)
                     ? mcumax.root_best_to
                     : mcumax.square_to;
  best_move.promotion_piece = 0;

  /* Hard legality gate: Ensure the selected best move is 100% guaranteed
   * to be in the verified legal moves list. If search produced an illegal
   * move, invalid square, or search artifact, pick the best legal fallback. */
  bool is_legal = false;
  uint32_t best_move_idx = 0;
  for (uint32_t i = 0; i < legal_count; i++) {
    if (legal_moves[i].from == best_move.from && legal_moves[i].to == best_move.to) {
      is_legal = true;
      best_move_idx = i;
      break;
    }
  }

  if (!is_legal) {
    mcumax_move fallback = mcumax_pick_safe_fallback_move();
    if (fallback.from == MCUMAX_SQUARE_INVALID)
      best_move = legal_moves[0];
    else
      best_move = fallback;
    for (uint32_t i = 0; i < legal_count; i++) {
      if (legal_moves[i].from == best_move.from && legal_moves[i].to == best_move.to) {
        best_move_idx = i;
        break;
      }
    }
    int32_t max_score = mcumax_root_eval.scores[best_move_idx];
    for (uint32_t i = 0; i < legal_count; i++) {
      if (mcumax_root_eval.scores[i] > max_score) {
        max_score = mcumax_root_eval.scores[i];
        best_move_idx = i;
        best_move = legal_moves[i];
      }
    }
  }

  mcumax_move chosen_move = best_move;

  if (!mcumax_probabilistic_selection_enabled) {
    /* Extreme mode / Deterministic mode:
     * Bypasses probabilistic selection and always picks the best evaluated move.
     * Sets 100% percentage on the best move and 0% on others. */
    chosen_move = best_move;
    for (uint32_t i = 0; i < legal_count; i++) {
      mcumax_root_eval.percentages[i] = 0.0f;
    }
    mcumax_root_eval.percentages[best_move_idx] = 100.0f;
  } else {
    /* ==================================================================== */
    /* Weighted random move selection based on centipawn score (cp >= 0)   */
    /* ==================================================================== */
    uint32_t candidate_indices[100];
    uint32_t candidate_count = 0;
    int64_t total_score = 0;

    for (uint32_t i = 0; i < legal_count; i++) {
      if (mcumax_root_eval.scores[i] >= 0) {
        candidate_indices[candidate_count++] = i;
        total_score += mcumax_root_eval.scores[i];
      }
    }

    if (candidate_count == 0) {
      /* Edge Case 1: No move has score >= 0 (all moves are negative).
       * Confirmed requirement: Pick the best move (highest score). */
      chosen_move = best_move;
      mcumax_root_eval.percentages[best_move_idx] = 100.0f;
    } else if (total_score == 0) {
      /* Edge Case 2: All candidate moves have score == 0 (total_score == 0).
       * Confirmed requirement: Pick uniformly at random among the 0-score moves. */
      float equal_pct = 100.0f / (float)candidate_count;
      for (uint32_t i = 0; i < candidate_count; i++) {
        mcumax_root_eval.percentages[candidate_indices[i]] = equal_pct;
      }
      uint32_t r = mcumax_rand() % candidate_count;
      chosen_move = legal_moves[candidate_indices[r]];
    } else {
      /* Standard Case:
       * + Cộng tổng giá trị centipawn của tất cả các nước đi >= 0
       * + Sau đó lấy tổng này chia cho 100, gọi là scale_a (a)
       * + Lấy toàn bộ các nước đi có giá trị >= 0 lần lượt chia cho scale_a
       * + Quyết định nước đi theo xác suất % bằng RNG mcumax_rand() */
      float scale_a = (float)total_score / 100.0f;
      float r_pct = ((float)mcumax_rand() / 32768.0f) * 100.0f; /* Uniform [0.0, 100.0) */
      float cumulative_pct = 0.0f;
      bool picked = false;

      for (uint32_t i = 0; i < candidate_count; i++) {
        uint32_t idx = candidate_indices[i];
        float pct = (float)mcumax_root_eval.scores[idx] / scale_a;
        mcumax_root_eval.percentages[idx] = pct;
        cumulative_pct += pct;
        if (!picked && r_pct < cumulative_pct) {
          chosen_move = legal_moves[idx];
          picked = true;
        }
      }
      if (!picked) {
        chosen_move = legal_moves[candidate_indices[candidate_count - 1]];
      }
    }
  }

  /* Detect whether the chosen move is a pawn promotion. We use the
   * stable snapshot because the working board's state at this point
   * has been restored by the search. */
  uint8_t moving_piece = mcumax.board[chosen_move.from];
  uint8_t piece_type = moving_piece & 0x07;
  if (piece_type == MCUMAX_PAWN_UPSTREAM || piece_type == MCUMAX_PAWN_DOWNSTREAM) {
    uint8_t to_rank = chosen_move.to >> 4;
    if (to_rank == 0 || to_rank == 7) {
      /* Run the 4-piece comparison and remember the choice. */
      mcumax_piece chosen = mcumax_select_promotion_piece(chosen_move, depth_max);
      chosen_move.promotion_piece = chosen;
      mcumax.last_promotion_piece = chosen;
    }
  }

  return chosen_move;
}

bool mcumax_play_move(mcumax_move move) {
  /* The search applies the move on the working board. If the move is
   * a promotion the search defaults to queen; we override the piece
   * type afterwards when the caller (or the engine itself, via the
   * promotion_piece field) requested a different piece. */
  bool played = mcumax_start_search(MCUMAX_PLAY_MOVE, move, 0, 0) ==
                MCUMAX_SCORE_MAX;

  if (!played)
    return false;

  /* Apply promotion override if requested. */
  if (move.promotion_piece != 0) {
    uint8_t piece = mcumax.board[move.to];
    uint8_t side = piece & 0x18;
    mcumax.board[move.to] =
        side | (move.promotion_piece & 0x07) | MCUMAX_PIECE_MOVED;
#ifdef MCUMAX_HASHING_ENABLED
    mcumax_recalculate_hash();
    /* Replace the most recent history entry so repetition detection
     * sees the corrected position. */
    if (mcumax.history_size > 0)
      mcumax.history_keys[mcumax.history_size - 1] = mcumax.hash_key;
#endif
  }

  /* Maintain derived state: castling rights, en passant target. */
  uint8_t moving_piece = mcumax.board[move.to];
  mcumax_update_castling_rights_for_move(move);
  mcumax_update_en_passant_for_move(move, moving_piece);

  mcumax_sync_stable_snapshot();
  return true;
}

void mcumax_set_callback(mcumax_callback callback, void *userdata) {
  mcumax.user_callback = callback;
  mcumax.user_data = userdata;
}

void mcumax_stop_search(void) { mcumax.stop_search = true; }

void mcumax_stop_search_core(int core_id) {
  if (core_id >= 0 && core_id < MCUMAX_NUM_CORES) {
    mcumax_instances[core_id].stop_search = true;
  }
}

void mcumax_set_contempt(int32_t contempt) { mcumax_contempt = contempt; }

void mcumax_promote_square(mcumax_square square, mcumax_piece piece_type) {
  if (square & MCUMAX_BOARD_MASK)
    return;
  uint8_t old_piece = mcumax.board[square];
  uint8_t side = old_piece & 0x18;
  mcumax.board[square] = side | (piece_type & 0x7) | MCUMAX_PIECE_MOVED;
#ifdef MCUMAX_HASHING_ENABLED
  mcumax_recalculate_hash();
  if (mcumax.history_size > 0)
    mcumax.history_keys[mcumax.history_size - 1] = mcumax.hash_key;
#endif
  mcumax_sync_stable_snapshot();
}

uint32_t mcumax_get_last_node_count(void) {
  return mcumax.last_search_node_count > 0 ? mcumax.last_search_node_count
                                           : mcumax.node_count;
}

/* ====================================================================== */
/* Migrated engine-layer query API                                        */
/* ====================================================================== */

uint8_t mcumax_get_castling_rights(void) { return mcumax.castling_rights; }

mcumax_square mcumax_get_en_passant_square(void) {
  return mcumax.stable_en_passant_square;
}

uint32_t mcumax_get_halfmove_clock(void) {
  return (uint32_t)mcumax.stable_halfmove_clock;
}

uint32_t mcumax_get_position_hash(void) {
  /* DJB2 hash combining stable board, side, en passant and castling
   * rights. Used by the app layer for threefold-repetition tracking
   * across the move history. */
  uint32_t hash = 5381;
  for (uint8_t sq = 0; sq < 0x80; sq++) {
    if ((sq & 0x88) == 0)
      hash = ((hash << 5) + hash) + (mcumax.stable_board[sq] & 0x1F);
  }
  hash = ((hash << 5) + hash) + mcumax.stable_current_side;
  hash = ((hash << 5) + hash) + mcumax.stable_en_passant_square;
  hash = ((hash << 5) + hash) + mcumax.castling_rights;
  return hash;
}

mcumax_square mcumax_find_king(uint8_t side) {
  return mcumax_find_king_stable(side);
}

bool mcumax_is_square_attacked(mcumax_square square, uint8_t attacker_side) {
  return mcumax_is_square_attacked_stable(square, attacker_side);
}

bool mcumax_is_in_check(uint8_t side) {
  mcumax_square king = mcumax_find_king_stable(side);
  if (king == MCUMAX_SQUARE_INVALID)
    return false;
  uint8_t opponent = side ^ 0x18;
  return mcumax_is_square_attacked_stable(king, opponent);
}

mcumax_square mcumax_get_check_square(uint8_t side) {
  mcumax_square king = mcumax_find_king_stable(side);
  if (king == MCUMAX_SQUARE_INVALID)
    return MCUMAX_SQUARE_INVALID;
  uint8_t opponent = side ^ 0x18;
  return mcumax_is_square_attacked_stable(king, opponent) ? king
                                                          : MCUMAX_SQUARE_INVALID;
}

bool mcumax_is_castling_move(mcumax_move move) {
  uint8_t p = mcumax.stable_board[move.from];
  if ((p & 0x07) != MCUMAX_KING)
    return false;
  int from_file = move.from & 0x07;
  int to_file = move.to & 0x07;
  int from_rank = move.from >> 4;
  int to_rank = move.to >> 4;
  return (from_rank == to_rank) && (abs(to_file - from_file) == 2);
}

bool mcumax_is_castling_move_valid(mcumax_move move) {
  if (!mcumax_is_castling_move(move))
    return true; /* non-castling moves are not the concern of this check */

  uint8_t moving_side = mcumax.stable_current_side;
  uint8_t enemy_side = moving_side ^ 0x18;

  if (mcumax_is_square_attacked_stable(move.from, enemy_side))
    return false;

  int from_file = move.from & 0x07;
  int to_file = move.to & 0x07;
  int rank = move.from >> 4;
  int direction = (to_file > from_file) ? 1 : -1;

  for (int f = from_file + direction; f != to_file; f += direction) {
    mcumax_square pass_sq = (rank << 4) | f;
    if (mcumax_is_square_attacked_stable(pass_sq, enemy_side))
      return false;
  }

  if (mcumax_is_square_attacked_stable(move.to, enemy_side))
    return false;
  return true;
}

bool mcumax_is_en_passant_move(mcumax_move move) {
  uint8_t p = mcumax.stable_board[move.from];
  int type = p & 0x07;
  if (type != MCUMAX_PAWN_UPSTREAM && type != MCUMAX_PAWN_DOWNSTREAM)
    return false;
  int from_file = move.from & 0x07;
  int to_file = move.to & 0x07;
  if (abs(to_file - from_file) != 1)
    return false;
  if (mcumax.stable_board[move.to] != MCUMAX_EMPTY)
    return false;
  return (move.to == mcumax.stable_en_passant_square);
}

bool mcumax_is_promotion_move(mcumax_move move) {
  uint8_t p = mcumax.stable_board[move.from];
  int type = p & 0x07;
  if (type != MCUMAX_PAWN_UPSTREAM && type != MCUMAX_PAWN_DOWNSTREAM)
    return false;
  uint8_t to_rank = move.to >> 4;
  return (to_rank == 0 || to_rank == 7);
}

bool mcumax_is_legal_move(mcumax_move move) {
  /* Simulate the move on a local copy of the board and test whether
   * the moving side's king ends up in check. We use the stable board
   * as the starting point so the result is correct even while a
   * search is in progress. */
  uint8_t sim_board[0x80 + 1];
  memcpy(sim_board, mcumax.stable_board, sizeof(sim_board));

  uint8_t moving_side = mcumax.stable_current_side;
  uint8_t moving_piece = sim_board[move.from];
  uint8_t type = moving_piece & 0x07;

  /* En passant: also remove the captured pawn. */
  bool is_ep = (type == MCUMAX_PAWN_UPSTREAM || type == MCUMAX_PAWN_DOWNSTREAM) &&
               (move.to == mcumax.stable_en_passant_square) &&
               (sim_board[move.to] == MCUMAX_EMPTY) &&
               ((move.to & 0x07) != (move.from & 0x07));
  if (is_ep) {
    mcumax_square captured_pawn_sq = move.to ^ 0x10;
    sim_board[captured_pawn_sq] = MCUMAX_EMPTY;
  }

  /* Move the piece. For promotions we just keep the original pawn
   * piece type for purposes of king-safety testing -- the piece type
   * does not affect whether our own king is in check. */
  sim_board[move.to] = moving_piece;
  sim_board[move.from] = MCUMAX_EMPTY;

  /* Find the moving side's king on the simulated board. */
  mcumax_square king_sq = MCUMAX_SQUARE_INVALID;
  if (type == MCUMAX_KING) {
    king_sq = move.to;
  } else {
    for (uint8_t sq = 0; sq < 0x80; sq++) {
      if (sq & MCUMAX_BOARD_MASK) {
        sq += 7;
        continue;
      }
      uint8_t p = sim_board[sq];
      if ((p & 0x18) == moving_side && (p & 0x07) == MCUMAX_KING) {
        king_sq = sq;
        break;
      }
    }
  }

  if (king_sq == MCUMAX_SQUARE_INVALID)
    return true;

  /* Test if the king is attacked by the enemy on the simulated board.
   * We use a local attacker-scan that consults sim_board directly. */
  uint8_t enemy_side = moving_side ^ 0x18;

  /* Knights. */
  int knight_offsets[8] = {-33, -31, -18, -14, 14, 18, 31, 33};
  for (int i = 0; i < 8; i++) {
    int s = king_sq + knight_offsets[i];
    if (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = sim_board[s];
      if ((p & 0x18) == enemy_side && (p & 0x07) == MCUMAX_KNIGHT)
        return false;
    }
  }
  /* Sliding orthogonal. */
  int rook_offsets[4] = {-16, -1, 1, 16};
  for (int i = 0; i < 4; i++) {
    int s = king_sq + rook_offsets[i];
    while (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = sim_board[s];
      if (p) {
        if ((p & 0x18) == enemy_side) {
          int t = p & 0x07;
          if (t == MCUMAX_ROOK || t == MCUMAX_QUEEN)
            return false;
        }
        break;
      }
      s += rook_offsets[i];
    }
  }
  /* Sliding diagonal. */
  int bishop_offsets[4] = {-17, -15, 15, 17};
  for (int i = 0; i < 4; i++) {
    int s = king_sq + bishop_offsets[i];
    while (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = sim_board[s];
      if (p) {
        if ((p & 0x18) == enemy_side) {
          int t = p & 0x07;
          if (t == MCUMAX_BISHOP || t == MCUMAX_QUEEN)
            return false;
        }
        break;
      }
      s += bishop_offsets[i];
    }
  }
  /* Pawns. */
  int pawn_offsets[4] = {-17, -15, 15, 17};
  for (int i = 0; i < 4; i++) {
    int s = king_sq + pawn_offsets[i];
    if (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = sim_board[s];
      if ((p & 0x18) == enemy_side) {
        int t = p & 0x07;
        if (t == MCUMAX_PAWN_UPSTREAM &&
            (king_sq == s - 15 || king_sq == s - 17))
          return false;
        if (t == MCUMAX_PAWN_DOWNSTREAM &&
            (king_sq == s + 15 || king_sq == s + 17))
          return false;
      }
    }
  }
  /* King adjacency. */
  int king_offsets[8] = {-17, -16, -15, -1, 1, 15, 16, 17};
  for (int i = 0; i < 8; i++) {
    int s = king_sq + king_offsets[i];
    if (!(s & MCUMAX_BOARD_MASK)) {
      uint8_t p = sim_board[s];
      if ((p & 0x18) == enemy_side && (p & 0x07) == MCUMAX_KING)
        return false;
    }
  }
  return true;
}

uint32_t mcumax_get_legal_moves(mcumax_move *buffer, uint32_t buffer_size) {
  mcumax_move temp[100];
  uint32_t count = mcumax_search_valid_moves(temp, 100);

  /* Filter out illegal castling first (matches the legacy app-layer
   * order: castling filter, then legality filter). */
  uint32_t filtered = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (mcumax_is_castling_move(temp[i]) && !mcumax_is_castling_move_valid(temp[i]))
      continue;
    if (!mcumax_is_legal_move(temp[i]))
      continue;
    if (filtered < buffer_size)
      buffer[filtered] = temp[i];
    filtered++;
  }
  return filtered;
}

/* ---- Draw detection (public, snapshot-based) ------------------------ */

bool mcumax_is_only_kings_left(void) {
  int piece_count = 0;
  for (uint8_t sq = 0; sq < 0x80; sq++) {
    if (sq & MCUMAX_BOARD_MASK) {
      sq += 7;
      continue;
    }
    uint8_t p = mcumax.stable_board[sq];
    if (p == MCUMAX_EMPTY)
      continue;
    piece_count++;
    if ((p & 0x07) != MCUMAX_KING)
      return false;
  }
  return piece_count == 2;
}

bool mcumax_is_insufficient_material(void) {
  return mcumax_insufficient_material_board(mcumax.stable_board) != 0;
}

bool mcumax_is_fifty_move_draw(void) {
  return mcumax.stable_halfmove_clock >= 100;
}

bool mcumax_is_threefold_repetition(void) {
  /* The engine's internal hash history spans the moves played through
   * mcumax_play_move() since the last mcumax_init() / set_fen_position().
   * The app layer may also keep its own DJB2 history via
   * mcumax_get_position_hash(); that history is the authoritative one
   * when the app uses undo/review. */
  if (mcumax.history_size == 0)
    return false;
  uint32_t current = mcumax.hash_key;
  uint32_t count = 1;
  for (int i = (int)mcumax.history_size - 2; i >= 0; i--) {
    if (mcumax.history_keys[i] == current)
      count++;
    if (count >= 3)
      return true;
  }
  return false;
}

mcumax_game_state_info_t mcumax_evaluate_game_state(void) {
  mcumax_game_state_info_t info;
  info.state = MCUMAX_GAME_ACTIVE;
  info.king_square = MCUMAX_SQUARE_INVALID;

  /* Draw conditions that do not depend on move count first. */
  if (mcumax_is_only_kings_left()) {
    info.state = MCUMAX_GAME_DRAW_ONLY_KINGS;
    return info;
  }
  int material_status = mcumax_insufficient_material_board(mcumax.stable_board);
  if (material_status == 1) {
    info.state = MCUMAX_GAME_DRAW_SAME_COLOR_BISHOPS;
    return info;
  } else if (material_status == 2) {
    info.state = MCUMAX_GAME_DRAW_INSUFFICIENT_MATERIAL;
    return info;
  }
  if (mcumax.stable_halfmove_clock >= 100) {
    info.state = MCUMAX_GAME_DRAW_FIFTY_MOVE;
    return info;
  }
  if (mcumax_is_threefold_repetition()) {
    info.state = MCUMAX_GAME_DRAW_THREEFOLD;
    return info;
  }

  /* Generate fully-legal moves; if none, it is checkmate or stalemate. */
  mcumax_move temp[100];
  uint32_t count = mcumax_get_legal_moves(temp, 100);

  uint8_t side_to_move = mcumax.stable_current_side;
  mcumax_square king_sq = mcumax_find_king_stable(side_to_move);
  info.king_square = king_sq;

  if (count == 0) {
    bool in_check = mcumax_is_in_check(side_to_move);
    if (in_check)
      info.state = MCUMAX_GAME_CHECKMATE;
    else
      info.state = MCUMAX_GAME_STALEMATE;
  }

  return info;
}

/* ====================================================================== */
/* Additional Engine API Implementations                                  */
/* ====================================================================== */

char mcumax_get_piece_char(mcumax_piece p) {
  switch (p & 0x07) {
  case MCUMAX_PAWN_UPSTREAM:
  case MCUMAX_PAWN_DOWNSTREAM:
    return '\0';
  case MCUMAX_KNIGHT:
    return 'N';
  case MCUMAX_BISHOP:
    return 'B';
  case MCUMAX_ROOK:
    return 'R';
  case MCUMAX_QUEEN:
    return 'Q';
  case MCUMAX_KING:
    return 'K';
  }
  return '\0';
}

int32_t mcumax_calculate_advantage(uint8_t side) {
  int32_t score = 0;
  for (uint8_t sq = 0; sq < 0x80; sq++) {
    if (sq & 0x88) {
      sq += 7;
      continue;
    }
    mcumax_piece p = mcumax.stable_board[sq];
    if (mcumax_is_empty_square(p))
      continue;
    int32_t val = 0;
    switch (p & 0x07) {
    case MCUMAX_PAWN_UPSTREAM:
    case MCUMAX_PAWN_DOWNSTREAM:
      val = 1;
      break;
    case MCUMAX_KNIGHT:
    case MCUMAX_BISHOP:
      val = 3;
      break;
    case MCUMAX_ROOK:
      val = 5;
      break;
    case MCUMAX_QUEEN:
      val = 9;
      break;
    }
    if ((p & 0x18) == side) {
      score += val;
    } else {
      score -= val;
    }
  }
  return score;
}

uint32_t mcumax_get_legal_moves_for_square(mcumax_square square, mcumax_move *buffer, uint32_t buffer_size) {
  mcumax_move tempBuffer[100];
  uint32_t totalCount = mcumax_get_legal_moves(tempBuffer, 100);

  uint32_t filteredCount = 0;
  for (uint32_t i = 0; i < totalCount; i++) {
    if (tempBuffer[i].from == square) {
      if (filteredCount < buffer_size) {
        buffer[filteredCount++] = tempBuffer[i];
      }
    }
  }
  return filteredCount;
}

uint32_t mcumax_get_last_move_evaluations(mcumax_move *moves, int32_t *scores,
                                          float *percentages, uint32_t max_count) {
  uint32_t n = mcumax_root_eval.count < max_count ? mcumax_root_eval.count : max_count;
  for (uint32_t i = 0; i < n; i++) {
    if (moves)
      moves[i] = mcumax_root_eval.legal_moves[i];
    if (scores)
      scores[i] = mcumax_root_eval.scores[i];
    if (percentages)
      percentages[i] = mcumax_root_eval.percentages[i];
  }
  return n;
}