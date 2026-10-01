/* ctool.c — exact Connect 4 solver + ply-bounded ground-truth export.
 *
 * WHY C AND NOT PYTHON, stated once so nobody re-litigates it: the bitboard is the
 * canonical representation for this game and it does not fit the Python story. Five
 * attempts were made at a Python 49-bit bitboard; every one ran and produced a
 * plausible number while being wrong. A representation you cannot reason about is a
 * measurement instrument you cannot check. In C it is one uint64_t and it is checkable.
 *
 * Board: 7 wide, 6 high, four in a row. 42 cells + 7 sentinel bits = 49.
 *
 *   bit  0..5  column 0, bottom to top      bit  6   sentinel
 *   bit  7..12 column 1, bottom to top      bit 13   sentinel
 *   ...
 *   bit 42..47 column 6, bottom to top      bit 48   sentinel
 *
 * TWO TRAPS IN THIS FILE, both of which cost real time, both now fixed and both worth
 * not re-introducing:
 *
 *   1. C precedence: `+` binds tighter than `<<`, so `b + (Board)1 << 27` means
 *      `(b + 1) << 27`, not `b + (1 << 27)`. It is accidentally correct on an empty board
 *      and silently wrong on every position after the first stone. Every shift of a cast
 *      constant is now parenthesised.
 *   2. The column masks were `(1 << (k*STRIDE)) & 0x7F`, which is not a column mask --
 *      for column 1 that is (1<<14) & 0x7F == 0. So every column except column 0 read as
 *      "height zero, always playable" and the search explored a board that does not exist.
 *
 * Neither produced a crash. Both produced a plausible number, and both were caught by the
 * two known-answer checks, which is the only reason they cost an afternoon and not a
 * published dataset.
 *
 * FNV-1a 64 is hand-rolled. The platform libc is inconsistent about this and the whole
 * project depends on it being identical everywhere, so it is spelled out here.
 *
 *   h = 0xcbf29ce484222325
 *   for each byte b: h ^= b; h *= 0x100000001b3
 *
 * Build:  cc -O3 -std=c11 -o ctool ctool.c
 * Run:    ./ctool [max_ply]
 *
 * The two known-answer checks run FIRST and are not skippable. If either fails this
 * program exits non-zero having printed which one. A fast wrong solver is worth nothing,
 * and a check that cannot fail is worse than no check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WIDTH   7
#define HEIGHT  6
#define CELLS   (WIDTH * HEIGHT)
#define BITS    ((CELLS + WIDTH - 1) / WIDTH + WIDTH)   /* 49 */
#define STRIDE  (HEIGHT + 1)                            /* 7 bits per column + sentinel */

typedef uint64_t Board;

/* Column c occupies bits [c*STRIDE, c*STRIDE+HEIGHT-1]; bit c*STRIDE+HEIGHT is the
 * sentinel that makes carries stop at the top of a column.
 *
 * The first version of this table was `(1 << (k*STRIDE)) & 0x7F` for k = 2..7, which is
 * not a column mask at all -- for column 1 it evaluates to (1<<14) & 0x7F == 0. So every
 * column except column 0 read as "height 0, always playable", and the search explored a
 * board that does not exist. Both known-answer checks returned 0.
 *
 * The masks are now written so the layout is visible: 6 playable bits per column, shifted
 * by c*STRIDE, and the sentinel as a separate single bit. */
#define PLAY_MASK   (((Board)1 << HEIGHT) - 1)                 /* 0x3F, 6 playable bits */
#define SENTINEL(c) ((Board)1 << ((c) * STRIDE + HEIGHT))
#define COL_PLAY(c) (PLAY_MASK << ((c) * STRIDE))

/* The bottom of each column, as a mask. 0x0101...01 with one bit per column start. */
#define BOTTOM_MASK ((Board)0x01010101010101ULL)

/* THE BIT LAYOUT, stated once, because this is where every bug so far lived.
 *
 *   column c, row r (0 = bottom)  ->  bit (c * STRIDE + r)
 *   playable bits of column c     ->  COL_PLAY(c) = 0x3F << (c*7),  i.e. c*7 .. c*7+5
 *   bit c*7+6                     ->  sentinel, so a carry cannot cross into column c+1
 *
 * The first version placed the first stone of a column at c*STRIDE + HEIGHT instead of
 * c*STRIDE + 0, so the stones and the mask were in different columns. `top()` and
 * `height_of()` disagreed about which column a stone was in, and nothing crashed.
 */
#define PLAY_BITS   HEIGHT
#define PLAY_MASK   (((Board)1 << PLAY_BITS) - 1)
#define COL_PLAY(c) (PLAY_MASK << ((c) * STRIDE))
#define SENTINEL(c) ((Board)1 << ((c) * STRIDE + HEIGHT))

/* One bit at the bottom of each column; the carrier for the top-mask form. */
static Board bottom_bits(void) {
    Board m = 0;
    for (int c = 0; c < WIDTH; c++) m |= (Board)1 << (c * STRIDE);
    return m;
}
static const Board BOTTOM_BITS = (Board)0x0002040810204081ULL;  /* bits 0,7,14,21,28,35,42 */

/* Top-mask form: which playable squares are occupied, one bit per column top. */
static inline Board top_mask(Board b) { return b + BOTTOM_BITS; }
static inline int  height_of(Board b, int c) { return __builtin_popcountll(b & COL_PLAY(c)); }
static inline int  is_playable(Board b, int c) { return height_of(b, c) < PLAY_BITS; }
/* The one playable square directly above the current column height. */
static inline Board top(Board b, int c) { return b + ((Board)1 << (c * STRIDE + height_of(b, c))); }

/* All the ways a drop could complete a line for `pos` given `mask` already filled.
 * This is the Pons trick: shifting pos by every direction and ANDing the four
 * consecutive-run forms gives, per cell, the squares that would complete a line. */
static inline Board compute_winning_position(Board pos, Board mask) {
    /* vertical */
    Board r = (pos << 1) & (pos << 2) & (pos << 3);
    /* horizontal */
    Board p = (pos << STRIDE) & (pos << (2 * STRIDE));
    r |= p & (pos << 3 * STRIDE);
    r |= p & (pos >> STRIDE);
    p = (pos >> STRIDE) & (pos >> 2 * STRIDE);
    r |= p & (pos << STRIDE);
    r |= p & (pos >> 3 * STRIDE);
    /* diagonal /  */
    p = (pos << (STRIDE + 1)) & (pos << (2 * (STRIDE + 1)));
    r |= p & (pos << 3 * (STRIDE + 1));
    r |= p & (pos >> (STRIDE + 1));
    p = (pos >> (STRIDE + 1)) & (pos >> 2 * (STRIDE + 1));
    r |= p & (pos << (STRIDE + 1));
    r |= p & (pos >> 3 * (STRIDE + 1));
    /* diagonal /  */
    p = (pos << (STRIDE - 1)) & (pos << (2 * (STRIDE - 1)));
    r |= p & (pos << 3 * (STRIDE - 1));
    r |= p & (pos >> (STRIDE - 1));
    p = (pos >> (STRIDE - 1)) & (pos >> 2 * (STRIDE - 1));
    r |= p & (pos << (STRIDE - 1));
    r |= p & (pos >> 3 * (STRIDE - 1));
    /* sentinels are never in mask, so this AND keeps them out of the result */
    return r & (mask | pos);
}

static inline int has_won(Board pos, Board mask) {
    return (compute_winning_position(pos, mask) & (mask | pos)) != 0;
}

/* Written as loops, not bit tricks. The single-arithmetic-shift forms of these are in the
 * literature and I got the constants wrong twice; a seven-iteration loop is obviously
 * correct and costs nothing next to the search. Cleverness here buys nothing. */
static inline int can_play_next(Board mask, Board pos) {
    (void)pos;
    for (int c = 0; c < WIDTH; c++) if (is_playable(mask, c)) return 1;
    return 0;
}

/* True when some column still has room for a piece that leaves 3+ in a line, i.e.
 * height <= HEIGHT-3, so a future four is not ruled out by this drop. */
static inline int possible_win_next(Board mask, Board pos) {
    (void)pos;
    for (int c = 0; c < WIDTH; c++) if (is_playable(mask, c) && height_of(mask, c) <= HEIGHT - 3) return 1;
    return 0;
}

static inline Board flip(Board b) {
    /* reverse the order of the 42 playable cells */
    Board r = 0;
    for (int c = 0; c < WIDTH; c++)
        for (int i = 0; i < HEIGHT; i++)
            if (b & ((Board)1 << (c * STRIDE + i)))
                r |= (Board)1 << (c * STRIDE + (HEIGHT - 1 - i));
    return r;
}

static int64_t nodes = 0;

/* ------------------------------------------------------------------ search */

static int negamax(Board mask, Board pos, int alpha, int beta, int depth_left);

/* ------------------------------------------------------------------ search */

static int negamax(Board mask, Board pos, int alpha, int beta, int depth_left);


/* ------------------------------------------------------------------ search
 *
 * POSE IS ALWAYS "the stones of the player to move". MASK is every stone. After the
 * current player drops at `move`, the opponent's stones are (mask|move) ^ pos, so the
 * recursive call passes that as the new pos. My first version tracked p0's stones and
 * p1's stones separately and then tried to negate p0's values inside a player-to-move
 * recursion; both known-answer checks came back 0. The gate caught it, which is the only
 * reason this bug cost ten minutes instead of a published dataset.
 */

#define TT_BITS  22
#define TT_SIZE  (1u << TT_BITS)
static uint32_t tt_key[TT_SIZE];
static int8_t   tt_val[TT_SIZE];
static uint8_t  tt_depth[TT_SIZE];
static uint8_t  tt_valid[TT_SIZE];
/* Bound stored SEPARATELY from the value. The first version encoded the bound in the value
 * field, using 0 for "upper bound" -- but 0 is also the legitimate game value for a draw.
 * So a stored draw came back as an upper bound of 0, which is a silent, plausible-looking
 * corruption. Two meanings for one field, exactly the class of bug this project keeps
 * finding. */
enum { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2 };
static uint8_t  tt_bound[TT_SIZE];

static inline uint64_t pos_key(Board pos, Board mask) {
    return (pos * 0x9E3779B97F4A7C15ULL) ^ (mask * 0xC2B2AE3D27D4EB4FULL);
}

static int move_score(Board mask, Board pos, int col) {
    Board t = top(mask, col);
    if (has_won(pos | t, mask | t)) return 1 << 20;          /* winning move, take it */
    /* prefer the middle, then reward a move that creates a threat */
    int centre = WIDTH / 2;
    int s = 1 << 10 - (col < centre ? centre - col : col - centre);
    Board m2 = mask | t, p2 = pos | t;
    Board possible = m2 | top_mask(p2);
    int threats = __builtin_popcountll(compute_winning_position(p2 | possible, possible)
                                       & possible & ~(m2 | p2));
    return s + threats * 64;
}

static int negamax(Board pos, Board mask, int alpha, int beta, int depth_left) {
    nodes++;

    if (depth_left == 0) return 0;

    /* No square left: the previous mover wins if their own stones form a line. */
    if (!can_play_next(mask, pos)) {
        Board prev = mask ^ pos;
        return has_won(prev, mask) ? -1 : 0;                 /* -1 loss, 0 draw */
    }

    uint64_t k = pos_key(pos, mask);
    uint32_t slot = (uint32_t)(k & (TT_SIZE - 1));
    int cutoff = 0;
    if (tt_valid[slot] && tt_key[slot] == (uint32_t)(k >> TT_BITS) && tt_depth[slot] >= depth_left) {
        int8_t v = tt_val[slot];
        if (tt_bound[slot] == BOUND_UPPER) {
            if (v > alpha) alpha = v;                     /* tighten alpha only */
        } else if (tt_bound[slot] == BOUND_LOWER) {
            if (v < beta) beta = v;                       /* tighten beta only  */
        } else {
            return v;                                     /* exact: no window change */
        }
        if (alpha >= beta) return alpha;
    }

    /* One square left: no branching. */
    if (!possible_win_next(mask, pos)) {
        int col = 0;
        for (int c = 0; c < WIDTH; c++) if (is_playable(mask, c)) { col = c; break; }
        Board move = top(mask, col);
        Board m2 = mask | move;
        if (has_won(pos | move, m2)) return 1;
        int r = -negamax(mask ^ pos, m2, -beta, -alpha, depth_left - 1);   /* opponent = mask ^ pos */
        if (r >= beta) return r;
        if (r > alpha) alpha = r;
        return alpha;
    }

    int moves[WIDTH], scores[WIDTH], n = 0;
    for (int c = 0; c < WIDTH; c++)
        if (is_playable(mask, c)) { moves[n] = c; scores[n] = move_score(mask, pos, c); n++; }
    for (int i = 1; i < n; i++) {                            /* insertion sort desc */
        int m = moves[i], s = scores[i], j = i - 1;
        while (j >= 0 && scores[j] < s) { moves[j+1]=moves[j]; scores[j+1]=scores[j]; j--; }
        moves[j+1] = m; scores[j+1] = s;
    }

    int a0 = alpha;
    for (int i = 0; i < n; i++) {
        int c = moves[i];
        Board move = top(mask, c);
        Board m2 = mask | move;
        if (has_won(pos | move, m2)) { alpha = 1; cutoff = 1; break; }
        int r = -negamax(mask ^ pos, m2, -beta, -alpha, depth_left - 1);   /* opponent = mask ^ pos */
        if (r >= beta) { alpha = r; cutoff = 1; break; }
        if (r > alpha)  alpha = r;
    }

    tt_valid[slot] = 1;
    tt_key[slot]   = (uint32_t)(k >> TT_BITS);
    tt_depth[slot] = (uint8_t)depth_left;
    tt_val[slot]   = (int8_t)alpha;
    tt_bound[slot] = (alpha <= a0) ? BOUND_LOWER
                    : (alpha >= beta) ? BOUND_UPPER : BOUND_NONE;
    return alpha;
}

/* value of the position from p0's point of view: +1 win, -1 loss, 0 unknown at horizon */
static int solve(Board mask, Board pos, int depth) {
    int v = negamax(pos, mask, -1, 1, depth);
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

/* ------------------------------------------------------------- known answers */

static void check_known(void) {
    int fail = 0;

    /* 1. the empty board is a loss for the first player. This is the known result and
     *    it is the single most useful single number in this game. */
    int empty = solve(0, 0, CELLS);
    printf("  check 1  empty board, value for P1 = %+d   expected -1   %s\n",
           empty, empty == -1 ? "OK" : "*** FAIL ***");
    if (empty != -1) fail = 1;

    /* 2. three stacked in the centre is a win for the first player. */
    Board m = 0, p = 0;
    for (int i = 0; i < 3; i++) { p |= top(m, 3); m |= top(m, 3); }
    int three = solve(m, p, CELLS);
    printf("  check 2  three in the centre, value for P1 = %+d   expected +1   %s\n",
           three, three == 1 ? "OK" : "*** FAIL ***");
    if (three != 1) fail = 1;

    if (fail) {
        printf("\n  A KNOWN-ANSWER CHECK FAILED. Not exporting. A fast wrong solver is worth\n"
               "  nothing, and an export built on it would be a dataset nobody can reproduce.\n");
        exit(2);
    }
    printf("  both known-answer checks passed\n");
}

/* ------------------------------------------------------------------- export */

static FILE *out;
static uint64_t digest = 0xcbf29ce484222325ULL;
static uint64_t rows_written = 0;
static int64_t  hist[3] = {0, 0, 0};      /* -1, 0, +1 */

static void fnv_push(const char *s) {
    for (unsigned char b; (b = (unsigned char)*s) != 0; s++) {
        digest ^= b;
        digest *= 0x100000001b3ULL;
    }
}

static int ply_hist[64];

static void emit(Board mask, Board pos, int value) {
    char line[64];
    int n = snprintf(line, sizeof line, "%llu %llu %+d\n",
                     (unsigned long long)mask, (unsigned long long)pos, value);
    fwrite(line, 1, n, out);
    fnv_push(line);
    rows_written++;
    hist[value + 1]++;
}

/* depth-limited export: every legal position with p0 to move at exactly `ply` plies,
 * one row per position, value from the FULL solve (not the depth bound) so the label is
 * the true game value and not an artefact of how deep we looked. */
/* Walk the tree and emit at exactly `target_ply`.
 *
 * NORMALISATION. The search is written in "player to move" perspective, so a naive emit
 * would alternate the meaning of the second column with the parity of the ply. The first
 * export did exactly that: at ply 1 every row read +1 and at ply 2 every row read -1, which
 * looked like a real signal and was in fact a sign flip. Two controls caught it -- every
 * 1-ply position must be a LOSS (the empty board is -1, so the first move hands the win
 * over), and the two stone counts must differ by at most one.
 *
 * So: normalise every emitted row to PLAYER ZERO. At odd plies p0 is to move and the
 * value is already p0's. At even plies p1 is to move, so swap the two bitboards and
 * negate. A dataset whose sign convention flips with row parity is a trap for every
 * downstream consumer, and the cost of fixing it here is one line. */
static void walk(Board mask, Board pos, int ply, int target_ply, int full_depth) {
    if (ply == target_ply) {
        int v = solve(mask, pos, full_depth);
        /* Ply 0 is PLAYER ZERO to move, so ODD plies are player one's turn. I had this
         * backwards the first time, which put the *fewer* stones in the p0 column on odd
         * plies -- a position where the second player has more pieces than the first, which
         * is impossible. The stone-count control caught it. */
        if (ply % 2 == 1) {            /* p1 to move -> rewrite as p0's point of view */
            Board p0 = mask ^ pos;
            emit(mask, p0, -v);
        } else {
            emit(mask, pos, v);
        }
        return;
    }
    for (int c = 0; c < WIDTH; c++) {
        if (!is_playable(mask, c)) continue;
        Board m2 = mask | top(mask, c);
        if (has_won(pos | top(mask, c), m2)) return;   /* game over: no children */
        /* The opponent's stones are mask ^ pos, NOT m2 ^ pos. m2 already contains the
         * move we just made, and that move belongs to the CURRENT player, so it has to
         * cancel: (mask|move) ^ (pos|move) == mask ^ pos. Using m2 ^ pos double-counts
         * the stone into the opponent's set. The two known-answer checks did not catch
         * it because has_won() fires before the recursion, but it corrupts every deeper
         * position -- and the export controls caught it immediately. */
        walk(m2, mask ^ pos, ply + 1, target_ply, full_depth);   /* opponent's view */
    }
}

int main(int argc, char **argv) {
    int max_ply = (argc > 1) ? atoi(argv[1]) : 6;

    printf("  ctool — Connect 4 exact solver, 7x6, four in a row\n\n");
    check_known();

    out = fopen("c4_ground_truth.txt", "w");
    if (!out) { perror("c4_ground_truth.txt"); return 1; }

    printf("\n  exporting positions at plies 1..%d (labels solved to full depth)\n", max_ply);
    int64_t total = 0;
    for (int ply = 1; ply <= max_ply; ply++) {
        int64_t before = rows_written;
        walk(0, 0, 0, ply, CELLS);
        int64_t at = rows_written - before;
        ply_hist[ply] = (int)at;
        total += at;
        printf("    ply %2d  %12lld positions\n", ply, (long long)at);
        fflush(out);
    }
    fclose(out);

    printf("\n  SELF-CHECKS ON THE EXPORT\n");
    /* Reopen for reading. The first version called rewind() on a file it had already
     * fclose()d -- undefined behaviour that silently read nothing, so control 1 reported
     * "OK (0 rows)". A control that passes on zero rows is not a control; it is the
     * vacuous test this project keeps finding in other people's code and now in mine. */
    {
        FILE *in = fopen("c4_ground_truth.txt", "r");
        if (!in) { perror("reopen"); return 4; }
        long nrows = 0, ok_1ply = 0, n1 = 0, ok_counts = 0, bad_counts = 0;
        int ok_first = 0;
        char ln[128];
        while (fgets(ln, sizeof ln, in)) {
            unsigned long long m, q; int v;
            if (sscanf(ln, "%llu %llu %d", &m, &q, &v) != 3) continue;
            nrows++;
            /* The invariant is |p0 - p1| <= 1, NOT popcount(mask) - popcount(p0) <= 1.
             * The second is "p1 has at most one stone", which is trivially true at ply 1
             * and false at ply 6 where the two players have three each -- so the first
             * version of this control rejected a correct export for being correct. */
            int total = __builtin_popcountll(m);
            int p0 = __builtin_popcountll(q);
            int p1 = total - p0;
            int d = p0 - p1; if (d < 0) d = -d;
            if (d > 1) bad_counts++; else ok_counts++;
            /* the very first row is ply 1: player zero has made one move, so p0 has the
             * stone and player one has none */
            if (nrows == 1) { if (p0 == 1 && p1 == 0) ok_first = 1; }
            if (total == 1) { n1++; if (v == -1) ok_1ply++; }
        }
        fclose(in);
        int pass = (nrows > 0) && (n1 == 7) && (ok_1ply == n1) && (bad_counts == 0)
                   && ok_first && (ok_counts + bad_counts == nrows);
        printf("    rows read back                 %ld   %s\n", nrows, nrows > 0 ? "OK" : "*** FAIL: read nothing ***");
        printf("    1-ply rows                     %ld  (expect exactly 7)  %s\n", n1, n1 == 7 ? "OK" : "*** FAIL ***");
        printf("    1-ply rows all valued -1       %ld/%ld  %s\n", ok_1ply, n1, ok_1ply == n1 && n1 > 0 ? "OK" : "*** FAIL ***");
        printf("    |p0| - |p1| <= 1 everywhere   %ld bad of %ld  %s\n", bad_counts, nrows, bad_counts == 0 ? "OK" : "*** FAIL ***");
        printf("    first row: p0 has 1, p1 has 0 %s\n", ok_first ? "OK" : "*** FAIL ***");
        if (!pass) {
            printf("\n  EXPORT CONTROLS FAILED. The file exists but is not trustworthy, and a\n"
                   "  dataset that fails its own controls is worse than no dataset.\n");
            return 3;
        }
        printf("    all export controls passed\n");
    }

    printf("\n  MANIFEST\n");
    printf("    positions         %llu\n", (unsigned long long)rows_written);
    printf("    value histogram   loss(-1) %lld   unknown(0) %lld   win(+1) %lld\n",
           (long long)hist[0], (long long)hist[1], (long long)hist[2]);
    printf("    FNV-1a 64 digest  0x%016llx\n", (unsigned long long)digest);
    printf("    nodes visited     %lld\n", (long long)nodes);
    printf("    TT entries        %u\n", TT_SIZE);
    printf("    file              c4_ground_truth.txt\n");
    printf("\n  convention: rows are `mask pos value` in decimal, p0 to move, value from\n"
           "  p0's point of view. 0 means the full-depth solve found neither a forced win\n"
           "  nor a forced loss within the horizon -- it is NOT a draw claim.\n");
    return 0;
}
