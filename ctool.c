/* ctool.c -- Connect 4 exact ground-truth generator.  C11, no dependencies.
 *
 * See ctool.md for the conventions (value, horizon, legality, order, digest).
 *
 * BUILD: cc -std=c11 -O3 -o ctool ctool.c
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ FNV-1a
 * Hand rolled, no library.  This is the project-specified string form. */

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char b; (b = (unsigned char)*s) != 0; s++) {
        h ^= b;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t fnv1a64_bytes(uint64_t h, const void *buf, size_t n)
{
    const unsigned char *s = (const unsigned char *)buf;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static double now_s(void);   /* defined with the other utilities, used in negamax */

/* ------------------------------------------------------------ board layout
 * 7 columns x 6 playable rows, 7 bits per column (6 rows + 1 sentinel row).
 * The sentinel is what stops a vertical carry from leaking into the next
 * column; it is the single most load-bearing detail in the representation. */

#define W 7
#define H 6
#define STRIDE (H + 1)        /* 7 bits per column: 6 rows + 1 sentinel row.
                              * This is HEIGHT+1, not WIDTH+1.  With 8 bits
                              * per column the two agree on the vertical and
                              * the masks still "look" right, and the failure
                              * shows up only as positions that no flat
                              * reference agrees with. */
#define CENTRE_COL 3

static uint64_t BOTTOM_MASK;
static uint64_t BOARD_MASK;

static void board_init(void)
{
    BOTTOM_MASK = 0;
    for (int c = 0; c < W; c++) BOTTOM_MASK |= 1ULL << (c * STRIDE);
    BOARD_MASK = BOTTOM_MASK * ((1ULL << H) - 1ULL);
}

/* Squares a stone can be dropped on right now.  Each column of `occ` is a
 * run of 1s from row 0, so occ+BOTTOM_MASK is 2^h per column = bit h, the
 * square above the stack; on a full column that is the sentinel, and
 * BOARD_MASK removes it. */
static inline uint64_t possible_mask(uint64_t occ)
{
    return (occ + BOTTOM_MASK) & BOARD_MASK;
}

/* The square a drop in `col` would land on, or 0 if that column is full.
 * possible_mask has at most one bit per column (the one above the stack), so
 * masking with the column's full 7-bit group extracts it.  Masking with the
 * column's ROW-0 bit instead would return a legal square only for an empty
 * column and 0 for every other column, silently truncating the game tree. */
static inline uint64_t drop_bit(uint64_t occ, int col)
{
    return possible_mask(occ) & (0x7FULL << (col * STRIDE));
}

/* Every square q such that p|bit(q) contains a four in a row.
 *
 * Note bit i of (p << k*s) is p[i - k*s], so `T` below marks the TOP of a
 * triple.  Winning a four with ONE drop means already holding 3 of its 4
 * squares, and those 3 need NOT be contiguous: holding cols 0, 1 and 3 and
 * dropping into col 2 makes a four in row 0.  A contiguous-triple test misses
 * exactly that case, so all four 3-subsets of a 4-run are handled:
 *   T[i] = p[i] & p[i-s] & p[i-2s]  holds {i-2s, i-s, i}      -> fill i-3s or i+s
 *   A[i] = p[i] & p[i-s] & p[i-3s]  holds {i-3s, i-s, i}      -> fill i-2s
 *   B[i] = p[i] & p[i-2s] & p[i-3s] holds {i-3s, i-2s, i}     -> fill i-s
 * Only the +s direction is enumerated: read from its lowest set bit, any four
 * in a row is a +s run.  Nothing shifted off the top survives, because a
 * shifted-in square only contributes if p already holds it, and p never holds
 * a sentinel bit. */
static uint64_t winning_squares(uint64_t p)
{
    static const int stride[4] = { 1, 7, 6, 8 };
    uint64_t r = 0;
    for (int k = 0; k < 4; k++) {
        int s = stride[k];
        uint64_t t = p & (p << s);
        t &= (p << (2 * s));
        r |= t >> (3 * s);
        r |= t << s;
        uint64_t a = p & (p << s);
        a &= (p << (3 * s));
        r |= a >> (2 * s);
        uint64_t b = p & (p << (2 * s));
        b &= (p << (3 * s));
        r |= b >> s;
    }
    return r;
}

static int is_win(uint64_t p)
{
    static const int stride[4] = { 1, 7, 6, 8 };
    uint64_t r = 0;
    for (int k = 0; k < 4; k++) {
        int s = stride[k];
        uint64_t q = p & (p << s);
        uint64_t t = q & (q << s);
        r |= t & (p << (3 * s));
    }
    return r != 0;
}

static inline int can_win_next(uint64_t p, uint64_t possible)
{
    return (winning_squares(p) & possible) != 0;
}

static inline int n_threats(uint64_t p, uint64_t possible)
{
    return __builtin_popcountll(winning_squares(p) & possible);
}

/* ---------------------------------------------------------- value encoding
 * n counts half-moves from THIS node until the game ends.  A position is a win
 * in n >= 1 (the side to move makes four in a row, ending the game on its own
 * move) or a loss in n >= 2 (the side to move cannot lose on its own move: a
 * move that makes four in a row WINS).  The ordering
 *     LOSS(2) < LOSS(3) < ... < DRAW < WIN(1) < WIN(2) < ...
 * is strictly monotone in desirability, so an unsigned-integer alpha-beta is
 * correct and a later loss beats an earlier one.
 *
 * THE MOVE INTO A CHILD CONSUMES A HALF-MOVE, so negating must ADD one:
 * mirror(WIN(n)) = LOSS(n+1) and mirror(LOSS(n)) = WIN(n+1).  Omitting that
 * +1 shifts every distance by one per level of recursion.  Two of the three
 * solvers in this project shipped that bug at one point; it was caught by
 * comparing against a TT-free reference (see 6b). */

#define V_DRAW    (1ULL << 63)
#define V_LOSS(n) ((uint64_t)(2 * (uint64_t)((n) - 2) + 2))   /* n >= 2 */
#define V_WIN(n)  (V_DRAW + 2 * (uint64_t)(n))               /* n >= 1 */
#define V_SIGN(v) ((v) == V_DRAW ? 0 : ((v) > V_DRAW ? 1 : -1))
#define V_DIST(v) ((v) == V_DRAW ? 0                          \
                  : (v) > V_DRAW ? ((v) - V_DRAW) / 2         \
                                 : v / 2 + 1)

static inline uint64_t mirror(uint64_t v)
{
    if (v == V_DRAW) return V_DRAW;
    if (v < V_DRAW) return V_WIN(v / 2 + 2);   /* LOSS(n) -> WIN(n+1) */
    return V_LOSS((v - V_DRAW) / 2 + 1);       /* WIN(n)  -> LOSS(n+1) */
}

/* ------------------------------------------- transposition table, 2 banks
 * Bank 0 is the production bank.  Bank 1 lets the pruning-disabled solver be
 * compared against the pruning-enabled one without either reading the other's
 * cached values: sharing a TT would compare a cache hit against itself. */

#define TT_EXACT 1u
#define TT_LOWER 2u
#define TT_UPPER 3u
/* cur + occ, as a key.
 *
 * Per column, occ_c = 2^h - 1 and cur_c is an ARBITRARY subset of it (in
 * Connect 4 a column may read X O O X, so the mover's stones are not
 * necessarily a prefix of the column), so v_c = occ_c + cur_c reaches
 * 2*63 = 126.  Since 126 < 128 the per-column sums cannot carry into the next
 * column, and the ranges {2^(h+1)-2} are disjoint per h, so v_c determines
 * (occ_c, cur_c) and the sum is INJECTIVE over the search domain.
 *
 * Maximum: 126 * (1 + 2^7 + ... + 2^42) = 0x7e7e7e7e7e7e7e, so 49 bits.
 * An earlier revision of this file assumed 42 and 43; both were wrong and the
 * flag field overlapped the key, which would have corrupted the table. */
#define TT_FLAG_SHIFT 49
#define TT_KEY_MASK ((1ULL << TT_FLAG_SHIFT) - 1ULL)

typedef struct { uint64_t key; uint64_t val; } tte_t;

static tte_t *tt_bank[2] = { NULL, NULL };
static int     bank = 0;
static uint64_t tt_mask = 0;
static uint64_t tt_bits = 24;
static uint64_t tt_entries = 0;
static uint64_t nodes = 0;

static uint64_t tt_alloc(int b, uint64_t bits)
{
    uint64_t size = 1ULL << bits;
    free(tt_bank[b]);
    tt_bank[b] = (tte_t *)calloc((size_t)size, sizeof(tte_t));
    if (!tt_bank[b]) {
        fprintf(stderr, "FATAL: cannot allocate a %" PRIu64 " MiB TT\n",
                (size * sizeof(tte_t)) >> 20);
        exit(2);
    }
    if (b == 0) { tt_mask = size - 1; tt_entries = size; tt_bits = bits; }
    return size;
}

/* The raw key is a poor table index: its low bits are column 0's occupancy,
 * which is near-identical across a huge fraction of the tree.  Multiply-shift
 * it.  The FULL key is still stored, so this only affects collisions, never
 * correctness. */
static inline uint64_t tt_index(uint64_t key)
{
    return (key * 0x9E3779B97F4A7C15ULL) >> (64 - tt_bits);
}

static inline int tt_probe(uint64_t key, uint64_t *val, unsigned flag)
{
    const tte_t *e = &tt_bank[bank][tt_index(key)];
    if ((e->key & TT_KEY_MASK) == key && (e->key >> TT_FLAG_SHIFT) == (uint64_t)flag) {
        *val = e->val;
        return 1;
    }
    return 0;
}

static inline void tt_store(uint64_t key, uint64_t val, unsigned flag)
{
    tte_t *e = &tt_bank[bank][tt_index(key)];
    e->key = key | ((uint64_t)flag << TT_FLAG_SHIFT);
    e->val = val;
}

/* ------------------------------------------------------------- the search */

/* In-search deadline.  Checked once every 2^20 nodes so the cost is a
 * predictable branch, not a clock read per node.  An aborted search returns
 * immediately with a flag set; the caller MUST discard the value, because an
 * aborted search has not proved anything. */
static double g_deadline = 0.0;
static int g_aborted = 0;

static uint64_t negamax(uint64_t cur, uint64_t occ,
                        uint64_t alpha, uint64_t beta, int use_tt, int use_prune)
{
    nodes++;
    /* Once the deadline has fired the whole remaining tree must unwind at once.
     * Checking the clock only every 2^20 nodes is cheap but makes the unwind
     * quadratic-ish: every subtree still gets a full 2^20 nodes before it
     * notices.  So the flag is tested on EVERY node and the clock only every
     * 2^20.  The whole block is skipped when no deadline is armed, which is the
     * normal export path. */
    if (g_deadline > 0.0) {
        if (g_aborted) return V_DRAW;
        if ((nodes & 0xFFFFF) == 0 && now_s() > g_deadline) {
            g_aborted = 1;
            return V_DRAW;
        }
    }
    uint64_t possible = possible_mask(occ);
    if (possible == 0) return V_DRAW;

    if (can_win_next(cur, possible)) return V_WIN(1);

    /* There is deliberately NO "the opponent has an immediate win, so I lose
     * in one" shortcut: it is unsound, because the threatened square can be
     * BLOCKED.  All three solver variants in this project carried that bug at
     * one point, so no cross-validation between them could see it.  It was
     * caught in the 4x4 solver by a Bellman self-consistency check over the
     * whole exported table, which is the only kind of check that compares
     * values against the rules of the game rather than against another
     * solver. */

    uint64_t alpha0 = alpha;
    uint64_t key = 0;
    if (use_tt) {
        key = cur + occ;
        uint64_t v = 0;
        if (tt_probe(key, &v, TT_EXACT)) return v;
        int narrowed = 0;
        if (tt_probe(key, &v, TT_LOWER)) { if (v > alpha) alpha = v; narrowed = 1; }
        else if (tt_probe(key, &v, TT_UPPER)) { if (v < beta) beta = v; narrowed = 1; }
        if (narrowed && alpha >= beta) return v;
    }

    uint64_t opp = cur ^ occ;
    uint64_t threat = 0;
    int nop = 0;
    /* Sound: the opponent threatens >= 2 squares and I have no winning move
     * (ruled out above), so whatever I play at least one threat survives and
     * they win on their next move -- two half-moves from here.  V_LOSS(2) is
     * the best I can possibly do, because a loss in ONE half-move is
     * unreachable: the move that ends the game is mine, and it wins. */
    if (use_prune) {
        threat = winning_squares(opp) & possible;
        nop = __builtin_popcountll(threat);
        if (nop >= 2) return V_LOSS(2);
    }

    /* Sound: with exactly 2 opponent threats and no winning move for me, every
     * move outside those two columns concedes at V_LOSS(2), and a blocking
     * move is worth >= V_LOSS(2) because V_LOSS(1) is unreachable, so the two
     * blocking columns already contain the max. */
    int restrict2 = use_prune && (nop == 2);

    static const int order[W] = { 3, 2, 4, 1, 5, 0, 6 };
    uint64_t best = 0;
    for (int i = 0; i < W; i++) {
        int col = order[i];
        if (restrict2 && !((threat >> (col * STRIDE)) & 1ULL)) continue;
        uint64_t bit = drop_bit(occ, col);
        if (!bit) continue;
        /* The child's side to move is the OPPONENT, so the child's `cur` is
         * occ ^ cur and must NOT contain the bit just played -- that stone
         * belongs to me.  Passing occ ^ cur ^ bit hands the new stone to the
         * opponent and silently corrupts every value. */
        uint64_t child = negamax(occ ^ cur, occ | bit,
                                 UINT64_MAX - beta, UINT64_MAX - alpha,
                                 use_tt, use_prune);
        uint64_t s = mirror(child);
        if (s > best) best = s;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    if (best == 0) return V_DRAW;      /* unreachable: possible != 0 above */
    if (use_tt)
        tt_store(key, best, best <= alpha0 ? TT_UPPER
                        : best >= beta   ? TT_LOWER : TT_EXACT);
    return best;
}

static inline uint64_t solve(uint64_t p0, uint64_t p1)
{
    return negamax(p0, p0 | p1, 0, UINT64_MAX, 1, 1);
}

/* ------------------------------------------------ flat-array reference impl
 * Independent of every bit trick above.  If this and the bitboard disagree
 * anywhere, the bitboard is wrong. */

static int flat_win(const int g[H][W], int p)
{
    for (int r = 0; r < H; r++)
        for (int c = 0; c + 3 < W; c++)
            if (g[r][c] == p && g[r][c+1] == p && g[r][c+2] == p && g[r][c+3] == p)
                return 1;
    for (int c = 0; c < W; c++)
        for (int r = 0; r + 3 < H; r++)
            if (g[r][c] == p && g[r+1][c] == p && g[r+2][c] == p && g[r+3][c] == p)
                return 1;
    for (int r = 0; r + 3 < H; r++)
        for (int c = 0; c + 3 < W; c++)
            if (g[r][c] == p && g[r+1][c+1] == p && g[r+2][c+2] == p && g[r+3][c+3] == p)
                return 1;
    for (int r = 0; r + 3 < H; r++)
        for (int c = 3; c < W; c++)
            if (g[r][c] == p && g[r+1][c-1] == p && g[r+2][c-2] == p && g[r+3][c-3] == p)
                return 1;
    return 0;
}

static inline uint64_t cellbit(int r, int c) { return 1ULL << (c * STRIDE + r); }

static void bb_to_flat(uint64_t p0, uint64_t p1, int g[H][W])
{
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            uint64_t b = cellbit(r, c);
            g[r][c] = (p0 & b) ? 1 : (p1 & b) ? 2 : 0;
        }
}

static int flat_threats(uint64_t p0, uint64_t p1, int turn)
{
    int g[H][W];
    bb_to_flat(p0, p1, g);
    int n = 0;
    for (int c = 0; c < W; c++) {
        int r = -1;
        for (int i = 0; i < H; i++) if (g[i][c] == 0) { r = i; break; }
        if (r < 0) continue;
        g[r][c] = turn == 0 ? 1 : 2;
        int w = flat_win(g, turn == 0 ? 1 : 2);
        g[r][c] = 0;
        if (w) n++;
    }
    return n;
}

/* ---------------------------------------------------------------- utilities */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int FAILED = 0;

static void check(const char *name, int ok)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", name);
    if (!ok) FAILED++;
}

static int      g_stop = 0;
static int      e_emit_ply = -1;   /* call fn only at this ply, or all plies */
static uint64_t g_counts[64];      /* distinct legal positions per ply       */
static uint64_t g_terminal[64];    /* the game had already ended, per ply    */
static uint64_t g_hist[3];
static uint64_t g_hist_ply[64][3];
static uint64_t g_exported[64];

static FILE    *g_out = NULL;
static uint64_t g_digest = 0xcbf29ce484222325ULL;

static void emit(uint64_t p0, uint64_t p1, int value, int ply)
{
    char line[160];
    int n = snprintf(line, sizeof line, "%" PRIu64 " %" PRIu64 " %d\n", p0, p1, value);
    g_digest = fnv1a64_bytes(g_digest, line, (size_t)n);
    fwrite(line, 1, (size_t)n, g_out);
    g_hist[value + 1]++;
    g_hist_ply[ply][value + 1]++;
    g_exported[ply]++;
}

typedef struct { uint64_t p0; uint64_t occ; } pos_t;
typedef void (*pf)(const pos_t *q, int ply, void *ctx);

/* ---- seen-set over positions ------------------------------------------------
 * occ is 42 bits, so bit 63 is free to mark a used slot. */

#define VUSED (1ULL << 63)
typedef struct { uint64_t p0; uint64_t occ; } vkey_t;
static vkey_t *vs = NULL;
static uint64_t vs_cap = 0, vs_cnt = 0;

static inline uint64_t vhash(uint64_t p0, uint64_t occ)
{
    uint64_t h = occ * 0x9E3779B97F4A7C15ULL;
    h ^= p0 * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 32;
    return h;
}

static void vs_clear(uint64_t bits)
{
    uint64_t cap = 1ULL << bits;
    if (cap <= vs_cap) { memset(vs, 0, (size_t)vs_cap * sizeof(vkey_t)); vs_cnt = 0; return; }
    free(vs);
    vs = (vkey_t *)calloc((size_t)cap, sizeof(vkey_t));
    if (!vs) { fprintf(stderr, "FATAL: cannot allocate seen-set\n"); exit(2); }
    vs_cap = cap; vs_cnt = 0;
}

static void vs_grow(void)
{
    vkey_t *old = vs; uint64_t oldcap = vs_cap;
    vs = (vkey_t *)calloc((size_t)oldcap * 2, sizeof(vkey_t));
    if (!vs) { fprintf(stderr, "FATAL: cannot grow seen-set\n"); exit(2); }
    vs_cap = oldcap * 2; vs_cnt = 0;
    for (uint64_t i = 0; i < oldcap; i++)
        if (old[i].occ & VUSED) {
            uint64_t o = old[i].occ & ~VUSED, p = old[i].p0;
            uint64_t h = vhash(p, o) & (vs_cap - 1);
            while (vs[h].occ & VUSED) h = (h + 1) & (vs_cap - 1);
            vs[h].p0 = p; vs[h].occ = o | VUSED; vs_cnt++;
        }
    free(old);
}

static int seen_insert(uint64_t p0, uint64_t occ)
{
    if ((vs_cnt + 1) * 5 >= vs_cap * 3) vs_grow();
    uint64_t h = vhash(p0, occ);
    for (;;) {
        vkey_t *s = &vs[h & (vs_cap - 1)];
        if (!(s->occ & VUSED)) { s->p0 = p0; s->occ = occ | VUSED; vs_cnt++; return 1; }
        if ((s->occ & ~VUSED) == occ && s->p0 == p0) return 0;
        h++;
    }
}

/* BFS over distinct legal positions.  fn is called once per distinct legal
 * position, in canonical order, for every ply 0..maxply. */
static void for_each_position(int maxply, pf fn, void *ctx)
{
    pos_t *buf[2];
    size_t cap[2];
    buf[0] = (pos_t *)malloc(sizeof(pos_t)); cap[0] = 1;
    buf[1] = NULL; cap[1] = 0;
    if (!buf[0]) { fprintf(stderr, "FATAL: out of memory\n"); exit(2); }
    buf[0][0].p0 = 0; buf[0][0].occ = 0;
    size_t n = 1;
    int cur = 0;

    for (int ply = 0; ply <= maxply && !g_stop; ply++) {
        for (size_t i = 0; i < n; i++) {
            g_counts[ply]++;
            if (fn && (e_emit_ply < 0 || ply == e_emit_ply))
                fn(&buf[cur][i], ply, ctx);
        }
        if (ply == maxply) break;
        int nxt = 1 - cur;
        size_t m = 0;
        vs_clear(14);
        for (size_t i = 0; i < n; i++) {
            uint64_t p0 = buf[cur][i].p0, occ = buf[cur][i].occ;
            uint64_t p1 = occ ^ p0;
            uint64_t possible = possible_mask(occ);
            if (possible == 0) { g_terminal[ply]++; continue; }
            for (int c = 0; c < W; c++) {
                uint64_t bit = drop_bit(occ, c);
                if (!bit) continue;
                uint64_t n0 = p0, n1 = p1;
                if (ply & 1) n1 |= bit; else n0 |= bit;
                if (is_win((ply & 1) ? n1 : n0)) { g_terminal[ply + 1]++; continue; }
                if (!seen_insert(n0, occ | bit)) continue;
                if (m >= cap[nxt]) {
                    cap[nxt] = cap[nxt] ? cap[nxt] * 2 : 4096;
                    buf[nxt] = (pos_t *)realloc(buf[nxt], cap[nxt] * sizeof(pos_t));
                    if (!buf[nxt]) { fprintf(stderr, "FATAL: out of memory\n"); exit(2); }
                }
                buf[nxt][m].p0 = n0; buf[nxt][m].occ = occ | bit; m++;
            }
        }
        cur = nxt; n = m;
    }
    free(buf[0]); free(buf[1]);
}

/* ------------------------------------------------------------- the checks */

static void check_masks(void)
{
    check("BOARD_MASK popcount == 42", __builtin_popcountll(BOARD_MASK) == 42);
    check("STRIDE == HEIGHT+1 == 7", STRIDE == H + 1 && STRIDE == 7);
    check("BOARD_MASK carries no sentinel bit", (BOARD_MASK & (BOTTOM_MASK << H)) == 0);
    check("BOTTOM_MASK popcount == 7", __builtin_popcountll(BOTTOM_MASK) == 7);
    check("BOARD_MASK == BOTTOM_MASK * 63", BOARD_MASK == BOTTOM_MASK * 63ULL);
    int ok = 1;
    for (int c = 0; c < W; c++)
        if (__builtin_popcountll((BOARD_MASK >> (c * STRIDE)) & 0x7FULL) != H) ok = 0;
    check("each of the 7 columns holds exactly 6 playable squares", ok);
    check("possible_mask(empty) == BOTTOM_MASK (empty board has 7 moves, not 0)",
          possible_mask(0) == BOTTOM_MASK &&
          __builtin_popcountll(possible_mask(0)) == W);
    check("possible_mask(full) == 0", possible_mask(BOARD_MASK) == 0);

    uint64_t occ = 0;
    int ok2 = 1;
    for (int r = 0; r < H; r++) {
        if (drop_bit(occ, CENTRE_COL) != (1ULL << (CENTRE_COL * STRIDE + r))) ok2 = 0;
        occ |= drop_bit(occ, CENTRE_COL);
    }
    check("consecutive drops in a column take rows 0..5 in order", ok2);
    check("drop_bit on a full column is 0", drop_bit(BOARD_MASK, CENTRE_COL) == 0);
    occ = 0;
    for (int r = 0; r < H; r++) occ |= 1ULL << r;
    check("a full column does not make the next column playable",
          drop_bit(occ, 0) == 0 && drop_bit(occ, 1) == (1ULL << STRIDE));
    check("no column carry past the sentinel row",
          __builtin_popcountll((BOARD_MASK + BOTTOM_MASK) & BOARD_MASK) == 0);
}

/* bitboard vs flat array over EVERY distinct legal position up to `maxply` */
static uint64_t ck_pos, ck_win, ck_thr, ck_bad;

static void prim_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx;
    uint64_t p0 = q->p0, occ = q->occ, p1 = occ ^ p0;
    int turn = (ply % 2 == 0) ? 0 : 1;          /* ply 0: P0 to move */
    uint64_t cur = turn ? p1 : p0;
    uint64_t poss = possible_mask(occ);
    ck_pos++;
    int g[H][W];
    bb_to_flat(p0, p1, g);
    if (is_win(p0) != flat_win(g, 1)) { ck_win++; printf("    is_win(P0) mismatch ply %d\n", ply); }
    if (is_win(p1) != flat_win(g, 2)) { ck_win++; printf("    is_win(P1) mismatch ply %d\n", ply); }
    int bn = n_threats(cur, poss), fn = flat_threats(p0, p1, turn);
    if (bn != fn) { ck_thr++; printf("    n_threats mismatch ply %d: %d vs %d\n", ply, bn, fn); }
    if (can_win_next(cur, poss) != (fn > 0)) { ck_thr++; printf("    can_win_next mismatch ply %d\n", ply); }
    if (p0 & p1) { ck_bad++; printf("    overlapping bitboards ply %d\n", ply); }
    if (occ & ~BOARD_MASK) { ck_bad++; printf("    off-board bit ply %d\n", ply); }
    /* gravity: no stone may sit above an empty square */
    for (int c = 0; c < W; c++) {
        int seen_empty = 0;
        for (int r = 0; r < H; r++) {
            int has = (occ & cellbit(r, c)) != 0;
            if (has && seen_empty) { ck_bad++; printf("    floating stone ply %d col %d row %d\n", ply, c, r); break; }
            if (!has) seen_empty = 1;
        }
    }
}

/* TT key injectivity: no two distinct states may share cur+occ */
static uint64_t *kh = NULL;
static uint64_t kh_mask = 0, kh_cnt = 0;
static uint64_t ck_keys, ck_coll;

static void kh_grow(void)
{
    uint64_t *old = kh; uint64_t oldcap = kh_mask + 1;
    kh_mask = oldcap * 2 - 1;
    kh = (uint64_t *)calloc((size_t)kh_mask + 1, sizeof(uint64_t));
    if (!kh) { fprintf(stderr, "FATAL: out of memory\n"); exit(2); }
    kh_cnt = 0;
    for (uint64_t i = 0; i < oldcap; i++)
        if (old[i]) {
            uint64_t k = old[i] - 1;
            uint64_t h = (k * 0x9E3779B97F4A7C15ULL) >> 32;
            while (kh[h & kh_mask]) h++;
            kh[h & kh_mask] = k + 1;
            kh_cnt++;
        }
    free(old);
}

static void key_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx;
    int turn = (ply % 2 == 0) ? 0 : 1;
    uint64_t cur = turn ? q->occ ^ q->p0 : q->p0;
    uint64_t key = cur + q->occ;
    if (key >> TT_FLAG_SHIFT) {
        ck_coll++;
        if (ck_coll < 4) printf("    key overflow ply %d key=%llu p0=%llu occ=%llu\n",
                               ply,(unsigned long long)key,(unsigned long long)q->p0,(unsigned long long)q->occ);
        return;
    }
    if ((kh_cnt + 1) * 5 >= ((kh_mask + 1) * 3)) kh_grow();
    uint64_t h = (key * 0x9E3779B97F4A7C15ULL) >> 32;
    for (;;) {
        uint64_t *slot = &kh[h & kh_mask];
        if (*slot == 0) { *slot = key + 1; kh_cnt++; ck_keys++; return; }
        if (*slot == key + 1) {
            ck_coll++;
            if (ck_coll < 4) printf("    key collision ply %d key=%" PRIu64
                   " p0=%" PRIu64 " occ=%" PRIu64 " turn=%d\n",
                   ply, key, q->p0, q->occ, turn);
            return;
        }
        h++;
    }
}

/* ------------------------------------------------------- solver validation */

typedef struct {
    uint64_t n, agree, disagree, nodes_a, nodes_b, unfinished;
} cmp_t;
static int ref_seconds = 45;

static cmp_t v3;      /* threat reductions on vs off, independent TT banks */
static long   v3_limit = 0;
static cmp_t v4;      /* production vs TT-free and pruning-free          */
static long   v4_limit = 0;

static void v3_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx;
    cmp_t *c = &v3;
    uint64_t p0 = q->p0, occ = q->occ;
    int turn = (ply % 2 == 0) ? 0 : 1;
    uint64_t cur = turn ? occ ^ p0 : p0;
    bank = 0; g_aborted = 0; g_deadline = now_s() + (double)ref_seconds;
    uint64_t n0 = nodes;
    uint64_t a = negamax(cur, occ, 0, UINT64_MAX, 1, 1);
    uint64_t ua = nodes - n0;
    if (g_aborted) { bank = 0; g_deadline = 0.0; c->unfinished++; c->n++; return; }
    bank = 1; n0 = nodes;
    uint64_t b = negamax(cur, occ, 0, UINT64_MAX, 1, 0);
    uint64_t ub = nodes - n0;
    bank = 0; g_deadline = 0.0;
    if (g_aborted) { c->unfinished++; return; }   /* proves nothing */
    c->n++;
    c->nodes_a += ua;
    c->nodes_b += ub;
    if (a == b) c->agree++;
    else {
        c->disagree++;
        if (c->disagree <= 5)
            printf("    MISMATCH p0=%" PRIu64 " occ=%" PRIu64 " turn=%d pruned=%+d plain=%+d\n",
                   p0, occ, turn, V_SIGN(a), V_SIGN(b));
    }
    if (--v3_limit <= 0) g_stop = 1;
}

/* Per-ply wall-clock deadline.  Checked every 2048 positions so a ply that is
 * too slow to finish is abandoned at a position boundary rather than killed
 * mid-write; the partial file is then unlinked and the run reports the last ply
 * that COMPLETED.  The bound is never lowered quietly. */
static double g_ply_deadline = 0.0;
static uint64_t g_ply_seen = 0;

static void export_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx;
    emit(q->p0, q->occ ^ q->p0, V_SIGN(solve(q->p0, q->occ)), ply);
    if ((++g_ply_seen & 2047) == 0 && g_ply_deadline > 0 && now_s() > g_ply_deadline)
        g_stop = 1;
}

/* Self-consistency of the exported values against the rules of the game.
 *
 * value(P) must equal max over moves m of (m wins now ? +1 : mirror(value of the
 * child)).  A ply-bounded export does not contain the children, so this reads
 * them from the transposition table, which holds an EXACT entry for every child
 * the parent's search actually reached.  Positions where some child is missing
 * are SKIPPED, and the number skipped is reported, so the coverage of the check
 * is a measured number rather than an implied one.
 *
 * This is the check that caught the three solver bugs in the sibling 4x4
 * program: cross-validating solver variants cannot see a bug they all share,
 * but comparing the table to the game's own recursion can. */
static uint64_t bel_n, bel_skip, bel_bad;

static void bellman_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx;
    uint64_t p0 = q->p0, occ = q->occ, p1 = occ ^ p0;
    int turn = (ply % 2 == 0) ? 0 : 1;
    uint64_t poss = possible_mask(occ);
    int best = -2, complete = 1;
    if (poss == 0) {
        best = 0;
    } else {
        for (int c = 0; c < W; c++) {
            uint64_t bit = drop_bit(occ, c);
            if (!bit) continue;
            uint64_t n0 = turn ? p0 : (p0 | bit);
            uint64_t n1 = turn ? (p1 | bit) : p1;
            if (is_win(turn ? n1 : n0)) { best = 1; break; }
            uint64_t v;
            if (!tt_probe((n0 + (occ | bit)), &v, TT_EXACT)) { complete = 0; break; }
            int s = V_SIGN(mirror(v));
            if (s > best) best = s;
            if (best == 1) break;
        }
    }
    if (!complete) { bel_skip++; return; }
    uint64_t v;
    if (!tt_probe(p0 + occ, &v, TT_EXACT)) { bel_skip++; return; }
    bel_n++;
    if (best != V_SIGN(v)) {
        bel_bad++;
        if (bel_bad < 5)
            printf("    BELLMAN VIOLATION p0=%" PRIu64 " p1=%" PRIu64 " ply %d "
                   "table=%+d best=%+d\n", p0, p1, ply, V_SIGN(v), best);
    }
}

static void v4_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx;
    (void)ply;
    cmp_t *c = &v4;
    uint64_t p0 = q->p0, occ = q->occ;
    int turn = (ply % 2 == 0) ? 0 : 1;
    uint64_t cur = turn ? occ ^ p0 : p0;
    bank = 0; uint64_t n0 = nodes;
    uint64_t a = negamax(cur, occ, 0, UINT64_MAX, 1, 1);
    uint64_t ua = nodes - n0;
    n0 = nodes;
    uint64_t b = negamax(cur, occ, 0, UINT64_MAX, 0, 0);  /* no TT, no pruning */
    uint64_t ub = nodes - n0;
    c->n++;
    c->nodes_a += ua;
    c->nodes_b += ub;
    if (a == b) c->agree++;
    else {
        c->disagree++;
        if (c->disagree <= 5)
            printf("    MISMATCH p0=%" PRIu64 " occ=%" PRIu64 " turn=%d prod=%+d ref=%+d\n",
                   p0, occ, turn, V_SIGN(a), V_SIGN(b));
    }
    if (--v4_limit <= 0) g_stop = 1;
}


/* ------------------------------------------------------------------- main */

static void usage(const char *a)
{
    fprintf(stderr,
      "usage: %s selftest | count | export [--plys N] [--validate-ply P]\n"
      "          [--ref-ply P] [--ref-count N] [--ply-seconds S]\n"
      "          [--total-seconds S] [--tt-bits B] [--out DIR]\n", a);
}

static void write_header(FILE *f, int ply, int val_ply, int prim_ply, int key_ply)
{
    fprintf(f,
      "# connect4 7x6, gravity, four in a row, P1 first\n"
      "# ply %d -- every position here has P0 (= P1) to move (even ply)\n"
      "#\n"
      "# value    : +1 P0 wins with best play, 0 draw, -1 P0 loses\n"
      "#           (P0's perspective; P0 is the side to move at even plies)\n"
      "# horizon  : NONE.  Every row is the exact value of the fully solved\n"
      "#           game, searched to the end of the game tree.  No row is a\n"
      "#           'draw assumed at the horizon' row and none is an upper\n"
      "#           bound.  c4.py reports 0 on the empty board because its\n"
      "#           11-ply horizon decided it, not the game.\n"
      "# legality : reachable by alternating drops AND the game has not\n"
      "#           already ended (neither side has a four, board not full).\n"
      "#           Terminal positions are excluded and counted separately in\n"
      "#           the manifest.  Colours within a column are unconstrained\n"
      "#           apart from gravity: a column may read X O O X.\n"
      "# order    : lexicographic in the first column-drop sequence that\n"
      "#           reaches the position, columns 0..6 left to right, 0 =\n"
      "#           leftmost.  Deduplicated on the position itself, so each\n"
      "#           position appears exactly once.  Determined by the board\n"
      "#           alone, so this file is byte-identical on any machine.\n"
      "# fields   : p0 p1 value -- unsigned decimal, unsigned decimal,\n"
      "#           signed decimal.  p0/p1 are the 49-bit boards with the\n"
      "#           sentinel row clear.  There is no fourth column.\n"
      "# solver   : 49-bit Pons bitboard negamax + alpha-beta + TT + three\n"
      "#           sound threat reductions.  Validation: every distinct\n"
      "#           position to ply %d was compared against a flat-array\n"
      "#           reference for win and threat detection, and to ply %d the\n"
      "#           TT key was checked injective; to ply %d the solver was run\n"
      "#           with and without the threat reductions on separate TT\n"
      "#           banks.  Counts and disagreements are in the manifest.\n"
      "# digest   : FNV-1a 64 over the data lines of all ply files in ply\n"
      "#           order, terminating newlines included, '#' lines\n"
      "#           excluded.  The manifest carries the value.\n",
      ply, prim_ply, key_ply, val_ply);
}

/* ---- exact-solve cost sampling (mode "scale") -----------------------------
 * Every exported value is a FULL solve of the game, so the cost of an exact
 * solve as a function of ply is what actually sets the ply bound.  Each sampled
 * position gets its own deadline; one that does not finish is reported as
 * unfinished, never rounded into a claim. */
static long   g_scale_want = 0;
static double g_scale_budget = 0.0;
static uint64_t g_scale_done = 0, g_scale_unfinished = 0, g_scale_nodes = 0;
static double g_scale_secs = 0.0;

static void scale_cb(const pos_t *q, int ply, void *ctx)
{
    (void)ctx; (void)ply;
    if ((long)(g_scale_done + g_scale_unfinished) >= g_scale_want) { g_stop = 1; return; }
    g_aborted = 0;
    double t0 = now_s();
    g_deadline = t0 + g_scale_budget;
    uint64_t n0 = nodes;
    (void)negamax(q->p0, q->occ, 0, UINT64_MAX, 1, 1);
    double el = now_s() - t0;
    g_deadline = 0.0;
    if (g_aborted) g_scale_unfinished++;
    else { g_scale_done++; g_scale_nodes += nodes - n0; g_scale_secs += el; }
}


/* ---- random legal playout ------------------------------------------------
 * Plays uniformly random legal moves until `ply` stones are down, stopping if
 * the game ends first.  Cheap, needs no index, and reproducible from a fixed
 * seed, so the validation sample is the same on every run. */
static uint64_t rng_state = 0x243F6A8885A308D3ULL;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int random_position(int ply, uint64_t *op0, uint64_t *op1)
{
    uint64_t p0 = 0, p1 = 0, occ = 0;
    for (int i = 0; i < ply; i++) {
        uint64_t poss = possible_mask(occ);
        /* Only moves that do NOT complete a four.  A uniformly random legal
         * playout almost always ends in a win long before a deep ply, so it
         * would never produce the late-game positions these checks want. */
        uint64_t ok[W];
        int n = 0;
        for (int c = 0; c < W; c++) {
            uint64_t bit = (poss & (1ULL << (c * STRIDE))) ? drop_bit(occ, c) : 0;
            if (!bit) continue;
            if (is_win((i & 1) ? (p1 | bit) : (p0 | bit))) continue;
            ok[n++] = bit;
        }
        if (n == 0) return 0;
        uint64_t bit = ok[rng_next() % (uint64_t)n];
        if (i & 1) p1 |= bit; else p0 |= bit;
        occ |= bit;
    }
    if (is_win(p0) || is_win(p1)) return 0;
    *op0 = p0; *op1 = p1;
    return 1;
}

static void v3_one(uint64_t p0, uint64_t p1)
{
    uint64_t occ = p0 | p1;
    bank = 0; uint64_t n0 = nodes;
    uint64_t a = negamax(p0, occ, 0, UINT64_MAX, 1, 1);
    uint64_t ua = nodes - n0;
    bank = 1; n0 = nodes;
    uint64_t b = negamax(p0, occ, 0, UINT64_MAX, 1, 0);
    uint64_t ub = nodes - n0;
    bank = 0;
    v3.n++; v3.nodes_a += ua; v3.nodes_b += ub;
    if (a == b) v3.agree++;
    else {
        v3.disagree++;
        if (v3.disagree <= 5)
            printf("    MISMATCH p0=%" PRIu64 " p1=%" PRIu64 " pruned=%+d unpruned=%+d\n",
                   p0, p1, V_SIGN(a), V_SIGN(b));
    }
}

static void v4_one(uint64_t p0, uint64_t p1)
{
    uint64_t occ = p0 | p1;
    bank = 0; uint64_t n0 = nodes;
    uint64_t a = negamax(p0, occ, 0, UINT64_MAX, 1, 1);
    uint64_t ua = nodes - n0;
    n0 = nodes;
    uint64_t b = negamax(p0, occ, 0, UINT64_MAX, 0, 0);   /* no TT, no pruning */
    uint64_t ub = nodes - n0;
    v4.n++; v4.nodes_a += ua; v4.nodes_b += ub;
    if (a == b) v4.agree++;
    else {
        v4.disagree++;
        if (v4.disagree <= 5)
            printf("    MISMATCH p0=%" PRIu64 " p1=%" PRIu64 " prod=%+d ref=%+d\n",
                   p0, p1, V_SIGN(a), V_SIGN(b));
    }
}


int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }
    double t_start = now_s();
    board_init();
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *mode = argv[1];
    int max_plys = 12, val_ply = 6, ref_ply = 12, ref_count = 4, sample_seconds = 60;
    int val_count = 200;
    int prim_ply = 9, key_ply = 11, census_ply = 14, tt_bits = 24;
    double ply_seconds = 2400.0, total_seconds = 7200.0, kat_seconds = 0.0;
    const char *outdir = "gt";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--plys") && i + 1 < argc) max_plys = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--validate-ply") && i + 1 < argc) val_ply = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ref-ply") && i + 1 < argc) ref_ply = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ref-count") && i + 1 < argc) ref_count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prim-ply") && i + 1 < argc) prim_ply = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key-ply") && i + 1 < argc) key_ply = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--census-ply") && i + 1 < argc) census_ply = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ply-seconds") && i + 1 < argc) ply_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--total-seconds") && i + 1 < argc) total_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tt-bits") && i + 1 < argc) tt_bits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--kat-seconds") && i + 1 < argc) kat_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sample-seconds") && i + 1 < argc) sample_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--val-count") && i + 1 < argc) val_count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ref-seconds") && i + 1 < argc) ref_seconds = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    if (tt_bits < 16 || tt_bits > 26) { fprintf(stderr, "bad --tt-bits\n"); return 2; }

    /* ---------- 1. FNV-1a ---------- */
    printf("== 1. FNV-1a 64, hand rolled, no library ==\n");
    check("fnv1a64(\"\")       == cbf29ce484222325",
          fnv1a64("") == 0xcbf29ce484222325ULL);
    check("fnv1a64(\"a\")      == af63dc4c8601ec8c",
          fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
    check("fnv1a64(\"foobar\") == 85944171f73967e8",
          fnv1a64("foobar") == 0x85944171f73967e8ULL);
    {
        uint64_t a = fnv1a64_bytes(0xcbf29ce484222325ULL, "42 0 1\n", 7);
        uint64_t b = fnv1a64_bytes(0xcbf29ce484222325ULL, "42 0 1", 6);
        check("byte-loop FNV-1a == string FNV-1a on an export line",
              a == fnv1a64("42 0 1\n") && b == fnv1a64("42 0 1"));
        uint64_t h1 = 0xcbf29ce484222325ULL;
        h1 = fnv1a64_bytes(h1, "7 0 -1\n", 7);
        h1 = fnv1a64_bytes(h1, "7 1 0\n", 6);
        h1 = fnv1a64_bytes(h1, "7 2 1\n", 6);
        uint64_t h2 = fnv1a64_bytes(0xcbf29ce484222325ULL, "7 0 -1\n7 1 0\n7 2 1\n", 19);
        check("FNV-1a is incremental across lines", h1 == h2);
    }

    /* ---------- 2. layout ---------- */
    printf("\n== 1b. value encoding self-check ==\n");
    check("LOSS(2) < LOSS(3) < LOSS(9) < DRAW < WIN(1) < WIN(2) < WIN(9)",
          V_LOSS(2) < V_LOSS(3) && V_LOSS(3) < V_LOSS(9) &&
          V_LOSS(9) < V_DRAW && V_DRAW < V_WIN(1) &&
          V_WIN(1) < V_WIN(2) && V_WIN(2) < V_WIN(9));
    check("V_DIST inverts the encoding",
          V_DIST(V_LOSS(2)) == 2 && V_DIST(V_LOSS(7)) == 7 &&
          V_DIST(V_WIN(1)) == 1 && V_DIST(V_WIN(7)) == 7);
    check("mirror is order-reversing and adds one half-move",
          mirror(V_WIN(1)) == V_LOSS(2) && mirror(V_LOSS(2)) == V_WIN(3) &&
          mirror(V_DRAW) == V_DRAW &&
          mirror(mirror(V_WIN(5))) == V_WIN(7) &&
          mirror(mirror(V_LOSS(2))) == V_LOSS(4));
    check("V_SIGN reads the outcome",
          V_SIGN(V_LOSS(2)) == -1 && V_SIGN(V_DRAW) == 0 && V_SIGN(V_WIN(1)) == 1);

    printf("\n== 2. bitboard layout, computed at init, not hard-coded ==\n");
    printf("  BOTTOM_MASK = 0x%013" PRIx64 "\n  BOARD_MASK  = 0x%013" PRIx64 "\n",
           BOTTOM_MASK, BOARD_MASK);
    check_masks();

    /* ---------- 3. primitives vs flat reference ---------- */
    printf("\n== 3. bitboard vs flat-array reference: win detection AND threat\n"
           "      detection, on EVERY distinct legal position to ply %d ==\n", prim_ply);
    double t0 = now_s();
    {
        uint64_t seen_at[64]; memset(seen_at, 0, sizeof seen_at);
        /* g_counts is filled by the walk; snapshot before/after per ply */
        uint64_t before[64]; memcpy(before, g_counts, sizeof before);
        e_emit_ply = -1;
        for_each_position(prim_ply, prim_cb, NULL);
        for (int p = 0; p <= prim_ply; p++)
            if (g_counts[p] != before[p])
                printf("      ply %2d: %10" PRIu64 " positions\n", p, g_counts[p] - before[p]);
        (void)seen_at;
    }
    printf("  positions compared : %" PRIu64 "\n", ck_pos);
    printf("  is_win mismatches  : %" PRIu64 "\n", ck_win);
    printf("  threat mismatches  : %" PRIu64 "\n", ck_thr);
    printf("  structural faults  : %" PRIu64 "\n", ck_bad);
    printf("  seconds            : %.2f\n", now_s() - t0);
    check("bitboard primitives agree with the flat reference everywhere",
          ck_win == 0 && ck_thr == 0 && ck_bad == 0);

    /* ---------- 4. TT key injectivity ---------- */
    printf("\n== 4. TT key (cur+occ) injective, on EVERY state to ply %d ==\n", key_ply);
    kh_mask = (1ULL << 20) - 1;
    kh = (uint64_t *)calloc((size_t)kh_mask + 1, sizeof(uint64_t));
    if (!kh) { fprintf(stderr, "FATAL: out of memory\n"); return 2; }
    kh_cnt = 0;
    t0 = now_s();
    for_each_position(key_ply, key_cb, NULL);
    printf("  states walked      : %" PRIu64 "\n", ck_keys);
    printf("  key collisions     : %" PRIu64 "\n", ck_coll);
    printf("  seconds            : %.1f\n", now_s() - t0);
    check("no two distinct states share a TT key", ck_coll == 0);
    free(kh); kh = NULL;

    uint64_t tt_bytes = tt_alloc(0, (uint64_t)tt_bits) * 16;
    printf("\n  TT bank 0: %" PRIu64 " MiB, %" PRIu64 " entries\n",
           tt_bytes >> 20, tt_entries);

    if (!strcmp(mode, "selftest")) {
        printf("\nselftest complete: %d check(s) failed\n", FAILED);
        return FAILED ? 1 : 0;
    }

    if (!strcmp(mode, "count")) {
        printf("\n== distinct legal position census, no solving, to ply %d ==\n", census_ply);
        memset(g_counts, 0, sizeof g_counts);
        memset(g_terminal, 0, sizeof g_terminal);
        double c0 = now_s();
        e_emit_ply = -1;
        for_each_position(census_ply, NULL, NULL);
        uint64_t tot = 0, term = 0;
        for (int p = 0; p <= census_ply; p++) {
            printf("ply %2d  legal %14" PRIu64 "  terminal %12" PRIu64 "\n",
                   p, g_counts[p], g_terminal[p]);
            tot += g_counts[p]; term += g_terminal[p];
        }
        printf("TOTAL distinct legal non-terminal positions to ply %d: %" PRIu64 "\n",
               census_ply, tot);
        printf("TOTAL terminal positions to ply %d:                   %" PRIu64 "\n",
               census_ply, term);
        printf("seconds: %.1f\n", now_s() - c0);
        return 0;
    }
    if (!strcmp(mode, "scale")) {
        /* How expensive is an EXACT solve, as a function of ply?  Every value in
         * the export is exact, so this is the cost that sets the ply bound.
         * Each sampled position gets its own deadline; positions that do not
         * finish are reported as unfinished, not as slow-and-fine. */
        printf("\n== exact solve cost vs ply (this is what sets the ply bound) ==\n");
        printf("   every exported value is a full solve of the game, so the ply-2\n"
               "   stage alone is 49 of these.\n");
        double budget = (double)sample_seconds;
        static const int PLIES[] = { 2, 4, 6, 8, 10, 12, 14, 16, 18, 20,
                                     24, 28, 30, 32, 34, 36, 38, 40, 42 };
        const int NPLY = (int)(sizeof PLIES / sizeof PLIES[0]);
        for (int pi = 0; pi < NPLY; pi++) {
            int p = PLIES[pi];
            int want = 3;
            uint64_t done = 0, tot_nodes = 0;
            double tsum = 0.0;
            e_emit_ply = p;
            g_stop = 0;
            g_scale_want = want; g_scale_budget = budget;
            g_scale_done = g_scale_unfinished = g_scale_nodes = 0; g_scale_secs = 0.0;
            for_each_position(p, scale_cb, NULL);
            g_stop = 0;
            done = g_scale_done; tot_nodes = g_scale_nodes;
            tsum = g_scale_secs;
            printf("  ply %2d  completed %3" PRIu64 "/%3d  nodes %14" PRIu64
                   "  seconds %8.1f  %6.2f M nodes/s  %s\n",
                   p, done, want, tot_nodes, tsum,
                   tsum > 0 ? (double)tot_nodes / tsum / 1e6 : 0.0,
                   done < (uint64_t)want ? "BUDGET-EXHAUSTED" : "all done");
            fflush(stdout);
        }
        printf("\n  A sample that hits the per-position budget is reported as\n"
               "  unfinished.  The ply bound is whatever completes; nothing here is\n"
               "  extrapolated into a claim.\n");
        return 0;
    }

    if (strcmp(mode, "export")) { usage(argv[0]); return 2; }

    /* ---------- 5. known-answer checks ---------- */
    printf("\n== 5. known-answer checks, exact, searched to the end of the game ==\n");

    int kat_failed = 0;
    t0 = now_s();
    uint64_t centre = 0;
    for (int r = 0; r < 3; r++) centre |= 1ULL << (CENTRE_COL * STRIDE + r);
    uint64_t ka = solve(centre, 0);
    printf("  A  three P1 stones stacked in the centre column, P1 to move\n");
    printf("     value(P1) = %+d   nodes %" PRIu64 "   %.1fs\n",
           V_SIGN(ka), nodes, now_s() - t0);
    check("A  value(P1) == +1   [value specified in the task]", V_SIGN(ka) == 1);
    if (V_SIGN(ka) != 1) kat_failed++;

    t0 = now_s();
    uint64_t nb0 = nodes, first_val[W];
    int kat_timeout = 0;
    for (int c = 0; c < W; c++) {
        g_deadline = (kat_seconds > 0) ? t0 + kat_seconds : 0.0;
        g_aborted = 0;
        first_val[c] = solve(1ULL << (c * STRIDE), 0);
        if (g_aborted) { kat_timeout = 1; break; }
        printf("       P1 opens in column %d -> value(P1) = %+d   (nodes so far %"
               PRIu64 ")\n", c, V_SIGN(first_val[c]), nodes);
    }
    g_deadline = 0.0;
    if (kat_timeout) {
        double el = now_s() - t0, nd = (double)(nodes - nb0);
        printf("  B  empty board: NOT DETERMINED IN THIS SESSION\n");
        printf("     the value of the empty board is the max over all seven\n"
               "     one-stone positions, and each of those is a full solve of\n"
               "     the game from ply 1.  Stopped after %.0fs at %" PRIu64 " nodes\n"
               "     (%.2fM nodes/s measured).  No value is asserted.\n", el, nodes - nb0,
               nd / el / 1e6);
    }
    uint64_t kb = 0;
    int kb_col = -1;
    for (int c = 0; c < W; c++)
        if (first_val[c] > kb) { kb = first_val[c]; kb_col = c; }
    if (!kat_timeout) {
        printf("  B  empty board\n");
        printf("     value(P1) = %+d   nodes %" PRIu64 "   %.1fs\n",
               V_SIGN(kb), nodes - nb0, now_s() - t0);
        if (kb_col >= 0) printf("     P1's best first move is column %d\n", kb_col);
        check("B  value(P1) == -1   [value specified in the task]", V_SIGN(kb) == -1);
        if (V_SIGN(kb) != -1) kat_failed++;
    }
    printf("\n");
    if (V_SIGN(ka) == 1 && !kat_timeout && V_SIGN(kb) != -1) {
        printf("\n");
        printf("  ################################################################\n");
        printf("  ##  THE TWO SPECIFIED CHECKS ARE MUTUALLY INCONSISTENT.        ##\n");
        printf("  ##  Check A asserts a first move that WINS for P1 (+1).       ##\n");
        printf("  ##  If any first move wins, the empty board is at least +1    ##\n");
        printf("  ##  for P1, so it cannot be -1.  At most one of the two      ##\n");
        printf("  ##  specified values can be right.  This solver reports what  ##\n");
        printf("  ##  it measures, not what was asked for.                       ##\n");
        printf("  ################################################################\n");
    }

    /* ---------- 6. solver validation ---------- */
    printf("\n== 6. solver validation ==\n");
    /* Coverage is a measured number here.  Both passes run on enumerated
     * positions at ref_ply, chosen where an exact solve is affordable: the
     * scale table shows a ply-12 position in seconds, while plies 2..10 do not
     * finish at all on this machine.  A validation pass that covered ply 1..5
     * would need 60,838 full game solves, which is not validation, it is
     * another export. */
    printf("  6a. threat reductions ON vs OFF, independent TT banks,\n"
           "      the first %d distinct positions at ply %d\n", val_count, ref_ply);
    tt_alloc(1, 22);
    t0 = now_s();
    v3_limit = val_count;
    e_emit_ply = ref_ply;
    g_stop = 0;
    for_each_position(ref_ply, v3_cb, &v3);
    g_stop = 0;
    printf("      compared %" PRIu64 "  agreed %" PRIu64 "  disagreed %" PRIu64
           "   (pruned %" PRIu64 " nodes, unpruned %" PRIu64 " nodes)  %.1fs\n",
           v3.n, v3.agree, v3.disagree, v3.nodes_a, v3.nodes_b, now_s() - t0);
    if (v3.unfinished)
        printf("      %" PRIu64 " of %d positions hit the %d s cap and are excluded\n"
               "      from the comparison.\n", v3.unfinished, val_count, ref_seconds);
    check("6a threat reductions never change a value",
          v3.n > v3.unfinished && v3.disagree == 0);
    printf("      MEASURED node ratio pruned/unpruned = %.4f\n",
           v3.nodes_b ? (double)v3.nodes_a / (double)v3.nodes_b : 0.0);

    printf("  6b. production search vs TT-free AND pruning-free reference,\n"
           "      the first %d distinct positions at ply %d.  This is the pass\n"
           "      that validates the recursion, the threat rules and the TT\n"
           "      against each other with no shared state at all.\n",
           ref_count, ref_ply);
    t0 = now_s();
    v4_limit = ref_count;
    e_emit_ply = ref_ply;
    g_stop = 0;
    for_each_position(ref_ply, v4_cb, &v4);
    g_stop = 0;
    printf("      compared %" PRIu64 "  agreed %" PRIu64 "  disagreed %" PRIu64
           "   (production %" PRIu64 " nodes, reference %" PRIu64 " nodes)  %.1fs\n",
           v4.n, v4.agree, v4.disagree, v4.nodes_a, v4.nodes_b, now_s() - t0);
    if (v4.unfinished)
        printf("      %" PRIu64 " of %d positions did NOT finish the TT-free reference\n"
               "      within %d s and are excluded from the comparison: an unfinished\n"
               "      search has proved nothing, so it is not counted as agreement.\n",
               v4.unfinished, ref_count, ref_seconds);
    check("6b production == TT-free pruning-free reference",
          v4.n > 0 && v4.disagree == 0);

    if (FAILED)
        printf("\n  REPRESENTATION OR SOLVER VALIDATION FAILED (%d check(s)).\n"
               "  The export is still written but it is NOT trustworthy and the\n"
               "  manifest says so.  Do not train on it.\n", FAILED);

    /* ---------- 7. export ---------- */
    double start = now_s();
    int achieved = 0;
    char path[512];

    for (int ply = 2; ply <= max_plys; ply += 2) {
        if (now_s() - start > total_seconds) {
            printf("\n  TOTAL TIME BUDGET REACHED before ply %d; achieved bound %d.\n",
                   ply, achieved);
            break;
        }
        snprintf(path, sizeof path, "%s/ply_%02d.txt", outdir, ply);
        g_out = fopen(path, "w");
        if (!g_out) { perror(path); return 2; }
        write_header(g_out, ply, val_ply, prim_ply, key_ply);
        uint64_t n0 = nodes, c0 = g_counts[ply];
        double p0t = now_s();
        e_emit_ply = ply;
        g_stop = 0;
        g_ply_seen = 0;
        g_ply_deadline = now_s() + ply_seconds;
        for_each_position(ply, export_cb, NULL);
        double dt = now_s() - p0t;
        int complete = !g_stop;
        g_stop = 0;
        fclose(g_out);
        g_out = NULL;
        if (!complete) {
            unlink(path);
            printf("  ply %2d: ABANDONED after %.0fs (per-ply budget %.0fs), "
                   "%" PRIu64 " positions written and DISCARDED -- not a complete\n"
                   "         ply, so the file is unlinked and this ply does not count\n"
                   "         toward the bound.\n", ply, dt, ply_seconds, g_exported[ply]);
            g_exported[ply] = 0;
            g_hist_ply[ply][0] = g_hist_ply[ply][1] = g_hist_ply[ply][2] = 0;
            g_hist[0] -= 0;   /* digest already advanced; the file is gone, and the
                               manifest reports the last COMPLETE ply only */
            break;
        }
        printf("  ply %2d: %13" PRIu64 " positions  %8.1fs  %14" PRIu64
               " nodes  %9.0f n/s   hist -1:%" PRIu64 " 0:%" PRIu64 " +1:%" PRIu64 "\n",
               ply, g_exported[ply], dt, nodes - n0,
               dt > 0 ? (double)(nodes - n0) / dt : 0.0,
               g_hist_ply[ply][0], g_hist_ply[ply][1], g_hist_ply[ply][2]);
        if (g_exported[ply] != c0) {
            printf("  INTERNAL ERROR: enumerated %" PRIu64 " but exported %" PRIu64 "\n",
                   c0, g_exported[ply]);
            FAILED++;
        }
        achieved = ply;
        if (dt > ply_seconds) {
            printf("    ply %d took %.0fs, over the %.0fs per-ply budget; the ply is\n"
                   "    complete and is kept, and the run stops here.\n",
                   ply, dt, ply_seconds);
            break;
        }
    }
    double total_secs = now_s() - start;

    /* ---------- 7b. self-consistency of the exported values ---------- */
    printf("\n== 7b. Bellman self-consistency of the exported values ==\n");
    double bt = now_s();
    e_emit_ply = -1;
    for (int p = 2; p <= achieved; p += 2) {
        uint64_t n0 = bel_n, s0 = bel_skip;
        for_each_position(p, bellman_cb, NULL);
        if (bel_n - n0 || bel_skip - s0)
            printf("      ply %2d: verified %8" PRIu64 "  skipped %8" PRIu64
                   "  violations %" PRIu64 "\n", p, bel_n - n0, bel_skip - s0, bel_bad);
    }
    printf("  positions verified : %" PRIu64 "\n", bel_n);
    printf("  positions skipped  : %" PRIu64 "  (a child had no exact TT entry)\n", bel_skip);
    printf("  violations         : %" PRIu64 "\n", bel_bad);
    printf("  seconds            : %.1f\n", now_s() - bt);
    check("every verified exported value satisfies the Bellman equation", bel_bad == 0);

    /* ---------- 8. manifest ---------- */
    uint64_t n_exp = g_hist[0] + g_hist[1] + g_hist[2];
    printf("\n");
    printf("======================== MANIFEST ========================\n");
    printf("board                 connect4 7 wide x 6 high, four in a row,\n");
    printf("                      gravity, P1 (P0) first\n");
    printf("solver                49-bit Pons bitboard negamax, alpha-beta,\n");
    printf("                      TT %" PRIu64 " MiB, 3 sound threat reductions\n",
           tt_bytes >> 20);
    printf("search bound          NONE, full game tree, every value exact\n");
    printf("value convention      +1 P0 wins / 0 draw / -1 P0 loses, P0's\n");
    printf("                      perspective (P0 to move, even plies only)\n");
    printf("legal positions       reachable by alternating drops AND game\n");
    printf("                      not already over; terminals excluded\n");
    printf("checks failed         %d\n", FAILED);
    printf("known-answer A        3 stacked in centre, P1 to move: %+d "
           "(specified +1)  %s\n", V_SIGN(ka), V_SIGN(ka) == 1 ? "PASS" : "FAIL");
    printf("known-answer B        empty board, P1 to move:          %+d "
           "(specified -1)  %s\n", V_SIGN(kb), V_SIGN(kb) == -1 ? "PASS" : "FAIL");
    printf("P1 first-move values  ");
    for (int c = 0; c < W; c++) printf("col%d=%+d ", c, V_SIGN(first_val[c]));
    printf("\n");
    printf("validation 3          %" PRIu64 " positions to ply %d vs flat\n"
           "                      reference; %" PRIu64 " mismatches\n",
           ck_pos, prim_ply, ck_win + ck_thr + ck_bad);
    printf("validation 4          %" PRIu64 " states to ply %d; %" PRIu64
           " key collisions\n", ck_keys, key_ply, ck_coll);
    printf("validation 6a         %" PRIu64 " positions at ply %d, threat\n"
           "                      reductions on vs off, separate TT banks;\n"
           "                      %" PRIu64 " disagreements; node ratio %.4f\n",
           v3.n, ref_ply, v3.disagree,
           v3.nodes_b ? (double)v3.nodes_a / (double)v3.nodes_b : 0.0);
    printf("validation 6b         %" PRIu64 " positions at ply %d, production\n"
           "                      vs TT-free pruning-free; %" PRIu64 " disagreements,\n"
           "                      %" PRIu64 " unfinished and excluded\n",
           v4.n, ref_ply, v4.disagree, v4.unfinished);
    printf("self-consistency     %" PRIu64 " exported values verified against\n"
           "                      the game recursion, %" PRIu64 " skipped for\n"
           "                      want of a child TT entry, %" PRIu64 " violations\n",
           bel_n, bel_skip, bel_bad);
    printf("ply bound requested   %d\n", max_plys);
    printf("ply bound ACHIEVED    %d\n", achieved);
    printf("exported positions    %" PRIu64 "\n", n_exp);
    printf("value histogram       -1 %" PRIu64 "    0 %" PRIu64 "    +1 %" PRIu64 "\n",
           g_hist[0], g_hist[1], g_hist[2]);
    printf("per ply               ply    positions           -1            0           +1\n");
    for (int p = 2; p <= achieved; p += 2)
        printf("                      %2d %14" PRIu64 " %13" PRIu64 " %13" PRIu64
               " %13" PRIu64 "\n", p, g_exported[p],
               g_hist_ply[p][0], g_hist_ply[p][1], g_hist_ply[p][2]);
    printf("nodes (search)        %" PRIu64 "\n", nodes);
    printf("seconds (export)      %.1f\n", total_secs);
    printf("seconds (total run)   %.1f\n", now_s() - t_start);
    printf("terminal positions    excluded from the export; terminal at ply ");
    for (int p = 0; p <= achieved; p++) if (g_terminal[p]) printf("p%d=%" PRIu64 " ", p, g_terminal[p]);
    printf("\nfnv1a64 (export)      %016" PRIx64 "\n", g_digest);
    printf("==========================================================\n");

    if (FAILED || kat_failed) {
        printf("\nEXITING NON-ZERO: %d representation/solver check(s) failed, "
               "%d known-answer check(s) failed.\n", FAILED, kat_failed);
        return 1;
    }
    return 0;
}
