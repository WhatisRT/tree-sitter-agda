#include "tree_sitter/parser.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define VEC_RESIZE(vec, _cap)                                                      \
    do {                                                                           \
        void *tmp = realloc((vec).data, (_cap) * sizeof((vec).data[0]));           \
        assert(tmp != NULL);                                                       \
        (vec).data = tmp;                                                          \
        (vec).cap = (_cap);                                                        \
    } while (0)

#define VEC_GROW(vec, _cap)                                                        \
    if ((vec).cap < (_cap)) {                                                      \
        VEC_RESIZE((vec), (_cap));                                                 \
    }

#define VEC_PUSH(vec, el)                                                          \
    do {                                                                           \
        if ((vec).cap == (vec).len) {                                              \
            VEC_RESIZE((vec), MAX(16, (vec).len * 2));                             \
        }                                                                          \
        (vec).data[(vec).len++] = (el);                                            \
    } while (0)

#define VEC_POP(vec) (vec).len--;

#define VEC_NEW { .len = 0, .cap = 0, .data = NULL }

#define VEC_BACK(vec) ((vec).data[(vec).len - 1])

#define VEC_FREE(vec)                                                              \
    {                                                                              \
        if ((vec).data != NULL)                                                    \
            free((vec).data);                                                      \
    }

#define VEC_CLEAR(vec) (vec).len = 0;

#define QUEUE_RESIZE(queue, _cap)                                                  \
    do {                                                                           \
        void *tmp = calloc((_cap), sizeof((queue).data[0]));                       \
        assert(tmp != NULL);                                                       \
        uint32_t count = (queue).tail - (queue).head;                              \
        for (uint32_t i = 0; i < count; i++) {                                     \
            ((uint16_t *)tmp)[i] = (queue).data[((queue).head + i) % (queue).cap]; \
        }                                                                          \
        if ((queue).data != NULL)                                                  \
            free((queue).data);                                                    \
        (queue).data = tmp;                                                        \
        (queue).head = 0;                                                          \
        (queue).tail = count;                                                      \
        (queue).cap = (_cap);                                                      \
    } while (0)

#define QUEUE_GROW(queue, _cap)                                                    \
    do {                                                                           \
        if ((queue).cap < (_cap)) {                                                \
            QUEUE_RESIZE((queue), (_cap));                                         \
        }                                                                          \
    } while (0)

#define QUEUE_PUSH(queue, el)                                                      \
    do {                                                                           \
        if ((queue).cap == 0) {                                                    \
            QUEUE_RESIZE((queue), 16);                                             \
        } else if ((queue).cap == ((queue).tail - (queue).head)) {                 \
            QUEUE_RESIZE((queue), (queue).cap * 2);                                \
        }                                                                          \
        (queue).data[(queue).tail % (queue).cap] = (el);                           \
        (queue).tail++;                                                            \
    } while (0)

#define QUEUE_POP(queue)                                                           \
    do {                                                                           \
        assert((queue).head < (queue).tail);                                       \
        (queue).head++;                                                            \
    } while (0)

#define QUEUE_FRONT(queue) (queue).data[(queue).head % (queue).cap]

#define QUEUE_EMPTY(queue) ((queue).head == (queue).tail)

#define QUEUE_NEW { .head = 0, .tail = 0, .cap = 0, .data = NULL }

#define QUEUE_FREE(queue)                                                          \
    do {                                                                           \
        if ((queue).data != NULL)                                                  \
            free((queue).data);                                                    \
    } while (0)

#define QUEUE_CLEAR(queue)                                                         \
    do {                                                                           \
        (queue).head = 0;                                                          \
        (queue).tail = 0;                                                          \
    } while (0)

enum TokenType {
    NEWLINE,
    INDENT,
    DEDENT,
    LAMBDA_NEWLINE,
    BLOCK_COMMENT,
    OPEN_IDIOM,    // `(|` followed by whitespace
    EMPTY_IDIOM,   // `(|)`
    AS_RENAME,     // see grammar.js externals
    // Marker externals — never emitted. valid_symbols[X_INDENT] is true exactly at
    // the matching block's INDENT push; the scanner reads it to set the flag below
    // and emits an ordinary INDENT. (Order MUST match grammar.js.)
    LET_INDENT,        // → LET_INLINE_FLAG
    DO_INDENT,         // → DO_BLOCK_FLAG
    LAM_WHERE_INDENT,  // → LAM_WHERE_FLAG
    COMMENT,       // emitted only for comments whose body contains NUL (0x00),
                   // which the built-in DFA stops at; other comments return false
    LAMBDA,  // must equal num_external_tokens: error-recovery sentinel
};

// Each indent-stack entry packs the column into the low 16 bits plus flag bits.
// INDENT_COLUMN masks off the flags (uint16_t cast), so flags never corrupt the
// column. The flags:
//   bit 31 INLINE_FLAG      INDENT emitted with no preceding newline (e.g. `let`
//                           inside `(let …)`); never gets a newline-driven dedent,
//                           so it is popped explicitly on `)` lookahead.
//   bit 30 PAREN_FLAG       pushed while NEWLINE was invalid — i.e. inside parens
//                           (`do` in `(do …)`), not a top-level module block; the
//                           `)` handler uses this to avoid closing module blocks.
//   bit 29 CONVERTED_FLAG   an inline entry converted to a block inside a
//                           brace/paren context (`λ where` in `record {…}`);
//                           suppresses the trailing NEWLINE (braces separate with `;`).
//   bit 28 DECL_INLINE_FLAG INLINE entry pushed at declaration level (NEWLINE valid
//                           at push); distinguishes a genuine `where`-block inline
//                           entry (needs a closing DEDENT) from a stranded `let` one.
//   bit 27 LET_INLINE_FLAG  marks a `let`-block entry, so the cross-newline close
//                           path can tell it from a `λ where` block (both close
//                           with `_dedent`) when ordering DEDENT emission.
//   bit 26 DO_BLOCK_FLAG    marks a do-block entry, so the `)` handler can cascade-
//                           close it (vs a `λ where` block — both non-inline PAREN).
//   bit 25 LAM_WHERE_FLAG   marks a `λ where` clause block, so it can be closed when
//                           a `where` keyword appears DEEPER than the clauses (lambda
//                           clauses have no where, so that `where` is the function's).
#define INLINE_FLAG       0x80000000u
#define PAREN_FLAG        0x40000000u
#define CONVERTED_FLAG    0x20000000u
#define DECL_INLINE_FLAG  0x10000000u
#define LET_INLINE_FLAG   0x08000000u
#define DO_BLOCK_FLAG     0x04000000u
#define LAM_WHERE_FLAG    0x02000000u
#define INDENT_COLUMN(x) ((uint16_t)((x) & 0x3fffffffu))
#define INDENT_IS_INLINE(x)      (((x) & INLINE_FLAG)      != 0)
#define INDENT_IS_PAREN(x)       (((x) & PAREN_FLAG)       != 0)
#define INDENT_IS_CONVERTED(x)   (((x) & CONVERTED_FLAG)   != 0)
#define INDENT_IS_DO(x)          (((x) & DO_BLOCK_FLAG)    != 0)
#define INDENT_IS_LAM_WHERE(x)   (((x) & LAM_WHERE_FLAG)   != 0)
// any `let`-declarations block, closed at the `in` keyword
#define INDENT_IS_LET(x)         (((x) & LET_INLINE_FLAG) != 0)
#define INDENT_IS_DECL_INLINE(x) (((x) & (INLINE_FLAG | DECL_INLINE_FLAG)) == \
                                   (INLINE_FLAG | DECL_INLINE_FLAG))
#define INDENT_MAKE(col, inl) ((uint32_t)(col) | ((inl) ? INLINE_FLAG : 0u))
#define INDENT_MAKE_PAREN(col)     ((uint32_t)(col) | PAREN_FLAG)
#define INDENT_MAKE_CONVERTED(col) ((uint32_t)(col) | PAREN_FLAG | CONVERTED_FLAG)

typedef struct {
    uint32_t len;
    uint32_t cap;
    uint32_t *data;
} indent_vec;

static indent_vec indent_vec_new() {
    indent_vec vec = VEC_NEW;
    vec.data = calloc(1, sizeof(uint32_t));
    vec.cap = 1;
    return vec;
}

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint16_t *data;
} token_queue;

static token_queue token_queue_new() {
    token_queue queue = QUEUE_NEW;
    queue.data = calloc(1, sizeof(uint16_t));
    queue.cap = 1;
    return queue;
}

typedef struct {
    indent_vec indents;
    uint32_t queued_dedent_count;
    // Counts non-DECL_INLINE INDENT entries popped in the non-inline-top while
    // loop (newline-crossing dedent) or the `else if (dedent)` while loop.
    // These represent expression-level inline blocks (e.g. `let` inside a `record
    // { … }`) that were stranded as zombies.  Drain as DEDENT when LD is valid,
    // or silently discard when D becomes valid without LD (the inline let's optional
    // DEDENT was already consumed as absent in this GLR branch).
    uint32_t queued_inline_let_count;
    // Counts DECL_INLINE INDENT entries popped by the non-inline-top while loop
    // (newline-crossing dedent) that belong to declaration-context `where`-blocks
    // (e.g. `foo = bar where go = λ where …`).  These must always drain as DEDENT
    // (never DEDENT) so that the outer _declaration_block receives its closing
    // DEDENT.  The drain retries on each call until D=1 — never zombie-discards.
    uint32_t queued_inline_where_count;
    // Counts popped INLINE do-block entries (`do stmt…` with the first statement on
    // the same line as `do`). Unlike a `let` zombie (queued_inline_let_count, which
    // discards when DEDENT is momentarily invalid), an inline do-block is a real
    // block whose last statement may be a multi-line `λ where`: its drain emits a
    // reducing NEWLINE (statement separator) to force the trailing application to
    // reduce, then the closing DEDENT. Never discards.
    uint32_t queued_inline_do_count;
    token_queue tokens;
    // Set when an inline INDENT entry is closed via a cross-newline DEDENT
    // (e.g. `(let b = x\n    ) → …`).  Prevents the scanner from pushing a
    // spurious same-line inline INDENT on the tokens that follow `)` on the
    // same line.  Cleared the next time the scanner crosses a newline.
    bool after_cross_newline_inline_dedent;
    // Minimum column of non-inline entries queued via queued_dedent_count in
    // the INLINE-TOP True Dedent or INLINE-TOP else-path while loops.  The
    // qdc drain must only fire when indent_length is STRICTLY BELOW this
    // column to prevent firing inside a nested block whose column is higher
    // than the qdc'd block.  Sentinel 0xFFFF means "no restriction".
    uint16_t queued_dedent_min_col;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// Returns true if the lookahead text matches `kw` followed by a word boundary
// (i.e. the next char is not alphanumeric, '_', or `'`).  The first character
// of kw must already match lexer->lookahead — callers do a one-char prefilter
// to avoid mark_end churn.  Calls mark_end so the recorded token boundary is
// at the start of the keyword; the lexer will reset the position on return
// since the scanner returns false (or doesn't commit) after peeking.
static bool peek_word_keyword(TSLexer *lexer, const char *kw) {
    lexer->mark_end(lexer);
    for (int i = 0; kw[i]; i++) {
        if (lexer->lookahead != (int32_t)(unsigned char)kw[i]) {
            return false;
        }
        lexer->advance(lexer, false);
    }
    int32_t c = lexer->lookahead;
    return !isalnum(c) && c != '_' && c != '\'';
}

// Pops indent-stack entries while the top column is strictly greater than
// `indent_length`, routing each pop to a queue counter:
//   - INLINE + DECL_INLINE  → queued_inline_where_count (drains as DEDENT)
//   - INLINE (non-DECL)     → queued_inline_let_count   (DEDENT or zombie-discard)
//   - non-INLINE            → queued_dedent_count       (DEDENT)
// Used by paths that queue pops for deferred emission. (The True-Dedent INLINE-TOP
// and same-line dedent paths use a different policy — all pops to qdc with min_col
// tightening — and do NOT call this.)
// `prev_sibling_col` is the column of the block the caller popped immediately above
// these (0xFFFF disables the let_in_do routing below); it lets us recognise the
// `let x = … λ where <deeper clauses>` do-statement shape (clause body deeper than
// the binding).
static inline void pop_outer_to_column_sib(Scanner *s, uint16_t indent_length,
                                           uint16_t prev_sibling_col) {
    uint16_t prev_popped_col = prev_sibling_col;
    while (s->indents.len > 0 &&
           indent_length < INDENT_COLUMN(VEC_BACK(s->indents))) {
        uint32_t e = VEC_BACK(s->indents);
        uint16_t e_col = INDENT_COLUMN(e);
        VEC_POP(s->indents);
        if (INDENT_IS_INLINE(e)) {
            if (INDENT_IS_DO(e)) {
                s->queued_inline_do_count++;
            } else if (INDENT_IS_DECL_INLINE(e)) {
                s->queued_inline_where_count++;
            } else if (s->indents.len > 0 &&
                       !INDENT_IS_INLINE(VEC_BACK(s->indents)) &&
                       INDENT_IS_PAREN(VEC_BACK(s->indents)) &&
                       !INDENT_IS_CONVERTED(VEC_BACK(s->indents)) &&
                       prev_popped_col != 0xFFFF &&
                       e_col < prev_popped_col) {
                // A non-DECL inline `let` (no `in` consumed) whose parent is a
                // do-block and whose column sits below the block popped above it:
                // a `let` as a do-statement (`let_in_do`) with `let x = … λ where`
                // clauses deeper than the binding. It closes with `_newline _dedent`,
                // so route to queued_inline_where_count. The `e_col < prev_popped_col`
                // guard excludes the inverted-indent shape (clauses shallower than the
                // binding), which still needs plain DEDENT routing.
                s->queued_inline_where_count++;
            } else {
                s->queued_inline_let_count++;
            }
        } else {
            s->queued_dedent_count++;
        }
        prev_popped_col = e_col;
    }
}

static inline void pop_outer_to_column(Scanner *s, uint16_t indent_length) {
    pop_outer_to_column_sib(s, indent_length, 0xFFFF);
}

// Increment queued_dedent_count for a popped entry and tighten
// queued_dedent_min_col so the qdc drain only fires at the appropriate column.
//
// For INLINE pops: the drain must fire only at or below `indent_length` (the
// column where the dedent was triggered), so spurious DEDENTs do not leak
// into a higher-column block on the same line.
//
// For non-INLINE pops: the drain must fire only at or below the popped
// entry's own column (`entry_col`).
//
// Used by the True-Dedent INLINE-TOP path and the same-line dedent path.
// Other qdc++ sites intentionally do NOT tighten min_col — see callers.
static inline void qdc_increment_with_min_col(Scanner *s, bool entry_is_inline,
                                              uint16_t entry_col,
                                              uint16_t indent_length) {
    s->queued_dedent_count++;
    uint16_t new_min = entry_is_inline ? indent_length : entry_col;
    if (new_min < s->queued_dedent_min_col) {
        s->queued_dedent_min_col = new_min;
    }
}

// Scan a block comment body after the opening "{-" has already been consumed.
// Handles nested {- ... -} pairs and treats {-# ... #-} as content.
// Caller must set result_symbol = BLOCK_COMMENT and call mark_end if needed.
static void scan_block_comment_body(TSLexer *lexer) {
    int depth = 1;
    bool prev_hash = false;
    while (depth > 0 && !lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        if (c == '{') {
            advance(lexer);
            prev_hash = false;
            if (lexer->lookahead == '-') {
                advance(lexer);
                if (lexer->lookahead == '#') {
                    advance(lexer);
                    prev_hash = false;
                } else {
                    depth++;
                    prev_hash = false;
                }
            }
        } else if (c == '-') {
            if (prev_hash) {
                advance(lexer);
                if (lexer->lookahead == '}') advance(lexer);
                prev_hash = false;
            } else {
                advance(lexer);
                if (lexer->lookahead == '}') {
                    advance(lexer);
                    depth--;
                }
                prev_hash = false;
            }
        } else if (c == '#') {
            advance(lexer);
            prev_hash = true;
        } else {
            advance(lexer);
            prev_hash = false;
        }
    }
}

// Returns true when the lookahead keyword should suppress an inline INDENT push
// so that declaration keyword stacking parses correctly.  Covers `in` (let
// expression closer) and the keywords that appear as the SECOND keyword in KS
// (keyword-stacking) patterns: `variable` in `private variable`, `instance` in
// `postulate instance` / `private instance` / `data D where instance`, `postulate`
// in `instance postulate` / `module X where postulate`, `record` in
// `abstract record`.  Also covers `abstract`, `data`, `codata`, `private` for
// symmetric / less-common stacking combos.
//
// Includes `mutual` because `module _ where mutual` stacks like other KS pairs:
// without suppression the inline INDENT for the outer module's block and the
// converted INDENT for `mutual`'s block would share one stack entry, leaving
// the outer module's `_declaration_block` without a matching DEDENT.
//
// Includes `module` (with lookahead) because `private module X where` stacks
// like other KS pairs: the outer modifier must take the `_declaration` path so
// that `module X where`'s content (on the next line at a lower column) can open
// a REAL INDENT instead of colliding with a spurious inline INDENT.
// However, module ALIASES (`module X = Y`) do NOT open a sub-block, so they do
// not suffer from the double-INDENT problem.  The case 'm' handler peeks past
// the module name: if `=` follows, it returns false (don't suppress) so that
// `where module C = Y\n  module D = Z` correctly uses `_declaration_block` to
// cover both aliases.  Only module definitions (`module X where …`) suppress.
//
// Includes `where` so that `foo = bar where go = λ where\n  x → x` does not
// receive a spurious inline INDENT at `where`'s column before the `where`
// keyword is consumed as `optional($.where)` for `foo`'s where-clause.
//
// Does NOT include `field`, `macro`, `open`, `opaque`, `syntax`, etc.
// because those keywords are not involved in any KS stacking pattern.
//
// Callers MUST check valid_symbols[INDENT] && !valid_symbols[DEDENT] &&
// valid_symbols[NEWLINE] before invoking — the NEWLINE guard restricts
// suppression to declaration-level contexts (where NEWLINE is a valid separator)
// and prevents firing inside expression contexts like `let instance` or
// `let postulate` where NEWLINE is not valid and the inline INDENT is needed.
// The function calls mark_end then advances to peek the keyword; since mark_end
// is set before any advance, the token boundary stays at the call site.
static bool scan_suppress_inline_indent(TSLexer *lexer, uint16_t indent_length) {
    if ((uint16_t)lexer->get_column(lexer) != indent_length) return false;
    lexer->mark_end(lexer);
    char w[20]; int n = 0;
    while (n < 19) {
        int32_t c = lexer->lookahead;
        if (!isalpha((unsigned char)c)) break;
        w[n++] = (char)c;
        lexer->advance(lexer, false);
    }
    w[n] = '\0';
    int32_t nb = lexer->lookahead;
    if (n == 0 || isalnum((unsigned char)nb) || nb == '_' || nb == '\'') return false;
    switch (w[0]) {
        case 'a': return strcmp(w, "abstract") == 0;
        case 'c': return strcmp(w, "codata") == 0;
        case 'd': return strcmp(w, "data") == 0;
        case 'i': return strcmp(w, "in") == 0 || strcmp(w, "instance") == 0;
        case 'm': {
            if (strcmp(w, "mutual") == 0) return true;
            if (strcmp(w, "module") != 0) return false;
            // Peek past the module name and any parameters: if '=' appears
            // before end-of-line / 'where', this is a module alias — no
            // double-INDENT problem, so do NOT suppress.  Module definitions
            // (module X where …) suppress as before.
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                lexer->advance(lexer, false);
            // Bail conservatively for attributes or end-of-line
            int32_t _mc = lexer->lookahead;
            if (_mc == 0 || _mc == '\n' || _mc == '\r')
                return true;
            // Skip module name
            while (lexer->lookahead != 0 && lexer->lookahead != ' ' &&
                   lexer->lookahead != '\t' && lexer->lookahead != '\n' &&
                   lexer->lookahead != '\r' && lexer->lookahead != '=' &&
                   lexer->lookahead != '(' && lexer->lookahead != '{')
                lexer->advance(lexer, false);
            // Scan the rest of the same line looking for '=' (alias) or
            // 'where' (definition).  Skip parameters, braces, etc.
            while (lexer->lookahead != 0 && lexer->lookahead != '\n' &&
                   lexer->lookahead != '\r') {
                if (lexer->lookahead == '=') return false; // alias
                // 'where' keyword → definition
                if (lexer->lookahead == 'w') {
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == 'h') {
                        lexer->advance(lexer, false);
                        if (lexer->lookahead == 'e') {
                            lexer->advance(lexer, false);
                            if (lexer->lookahead == 'r') {
                                lexer->advance(lexer, false);
                                if (lexer->lookahead == 'e') return true;
                            }
                        }
                    }
                    continue;
                }
                lexer->advance(lexer, false);
            }
            return true; // no '=' found → suppress
        }
        case 'p': return strcmp(w, "postulate") == 0 || strcmp(w, "private") == 0;
        case 'r': return strcmp(w, "record") == 0;
        case 'v': return strcmp(w, "variable") == 0;
        case 'w': return strcmp(w, "where") == 0;
        default:  return false;
    }
}

bool tree_sitter_agda_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    if (getenv("TS_AGDA_DEBUG")) {
        fprintf(stderr, "[scan] col=%u eof=%d lk=%d vN=%d vI=%d vD=%d vLN=%d vLI=%d stack=[",
                lexer->get_column(lexer),
                lexer->eof(lexer),
                lexer->lookahead,
                valid_symbols[NEWLINE], valid_symbols[INDENT], valid_symbols[DEDENT],
                valid_symbols[LAMBDA_NEWLINE], valid_symbols[LET_INDENT]);
        for (uint32_t i = 0; i < scanner->indents.len; i++) {
            uint32_t e = scanner->indents.data[i];
            fprintf(stderr, " %u%s%s%s%s%s%s%s",
                    INDENT_COLUMN(e),
                    INDENT_IS_INLINE(e) ? "I" : "",
                    INDENT_IS_PAREN(e) ? "P" : "",
                    INDENT_IS_CONVERTED(e) ? "C" : "",
                    INDENT_IS_DECL_INLINE(e) ? "D" : "",
                    INDENT_IS_LET(e) ? "L" : "",
                    INDENT_IS_DO(e) ? "O" : "",
                    INDENT_IS_LAM_WHERE(e) ? "W" : "");
        }
        fprintf(stderr, " ] qdc=%u qilc=%u qiwc=%u qidc=%u min=%u\n",
                scanner->queued_dedent_count, scanner->queued_inline_let_count,
                scanner->queued_inline_where_count, scanner->queued_inline_do_count,
                scanner->queued_dedent_min_col);
    }
    // Queued tokens from a previous call must be emitted before any other
    // processing.  Skipping this check caused queued DEDENT/NEWLINE pairs to
    // be silently dropped when the lookahead was `{` and BLOCK_COMMENT was
    // valid: the block-comment check below would fire first and return false,
    // leaving the queue non-empty but never draining it.
    if (!QUEUE_EMPTY(scanner->tokens)) {
        goto done_enqueue;
    }

    // Handle _BLOCK_COMMENT: nested {- ... -} comments with proper depth tracking.
    // Only fires when no DEDENT is needed at the current column — if the current
    // column is below the indent-stack top and DEDENT is valid, layout processing
    // must close the block before the block comment is emitted.
    // Handles null bytes and other control characters that confuse regex-based
    // block comment matching, and correctly tracks nested {- ... -} pairs.
    // Only intercepts when lookahead is '{'; otherwise falls through so that
    // LAMBDA/NEWLINE/INDENT/DEDENT processing still runs.
    if (valid_symbols[BLOCK_COMMENT] && lexer->lookahead == '{') {
        uint16_t _col = (uint16_t)lexer->get_column(lexer);
        uint16_t _top = INDENT_COLUMN(VEC_BACK(scanner->indents));
        bool _needs_dedent = valid_symbols[DEDENT] && _col < _top;
        if (!_needs_dedent) {
            advance(lexer);
            if (lexer->lookahead == '-') {
                advance(lexer);
                if (lexer->lookahead == '#') {
                    return false; // {-# is a pragma, position reset
                }
                scan_block_comment_body(lexer);
                lexer->result_symbol = BLOCK_COMMENT;
                return true;
            }
            return false; // not {-: position reset, fall through
        }
        // _needs_dedent: fall through to layout processing
    }

    // Handle _OPEN_IDIOM / _EMPTY_IDIOM: ASCII idiom bracket tokens.
    // Per Agda Issue #2186, spaces are required after `(|` to form an idiom
    // bracket, so `(|n|)` is a regular parenthesized expression, not an idiom.
    // Only emit OPEN_IDIOM when `(|` is followed by whitespace or EOF.
    // Emit EMPTY_IDIOM for the three-char sequence `(|)`.
    if ((valid_symbols[OPEN_IDIOM] || valid_symbols[EMPTY_IDIOM]) &&
        lexer->lookahead == '(') {
        uint16_t _col = (uint16_t)lexer->get_column(lexer);
        uint16_t _top = INDENT_COLUMN(VEC_BACK(scanner->indents));
        bool _needs_dedent = valid_symbols[DEDENT] && _col < _top;
        if (!_needs_dedent) {
            advance(lexer);  // consume '('
            if (lexer->lookahead == '|') {
                advance(lexer);  // consume '|'
                if (lexer->lookahead == ')' && valid_symbols[EMPTY_IDIOM]) {
                    advance(lexer);  // consume ')'
                    lexer->mark_end(lexer);
                    lexer->result_symbol = EMPTY_IDIOM;
                    return true;
                }
                if (valid_symbols[OPEN_IDIOM]) {
                    int32_t c = lexer->lookahead;
                    // Only emit OPEN_IDIOM when followed by whitespace or EOF.
                    // If followed by an identifier character, (|x...) is a
                    // regular paren around an identifier starting with '|'.
                    bool next_is_id = (c != 0 && c != '\n' && c != '\r' &&
                                       c != ' ' && c != '\t' &&
                                       c != ';' && c != '.' && c != '"' &&
                                       c != '(' && c != ')' &&
                                       c != '{' && c != '}' && c != '@');
                    if (!next_is_id) {
                        lexer->mark_end(lexer);
                        lexer->result_symbol = OPEN_IDIOM;
                        return true;
                    }
                }
            }
            return false;  // not (| or not idiom context: reset position
        }
    }

    // Handle _AS_RENAME: the `as` keyword that starts an as_clause.  Emitted only
    // when the module-name that follows `as` is NOT itself trailed by another `as`
    // word.  This resolves `open import M as as as` correctly: the first `as` is
    // a module arg (plain identifier), the second fires AS_RENAME, and the third
    // is the module alias name.
    //
    // Logic:  when lookahead is 'a':
    //   1. Consume 'a', 's'.  Confirm 's' is next and that this forms a whole word
    //      (next char after 's' is whitespace or a known delimiter).
    //   2. mark_end — the AS_RENAME token spans just the two chars `as`.
    //   3. Skip whitespace; consume the module-name identifier (all non-whitespace,
    //      non-delimiter chars including dots for qualified names).
    //   4. Skip whitespace after the name.
    //   5. If the next thing starts with 'a','s'+ word-boundary (another `as` token)
    //      → return false (this position should be an ordinary identifier, not the
    //        start of the as_clause).
    //   6. Otherwise → emit AS_RENAME.
    //
    // When we return false, tree-sitter resets the lexer to the position before the
    // scanner was called, so all the peek-advances are rolled back.
    if (valid_symbols[AS_RENAME] && lexer->lookahead == 'a') {
        advance(lexer);  // consume 'a'
        if (lexer->lookahead == 's') {
            advance(lexer);  // consume 's'
            // Confirm word boundary — `as` must not be part of a longer identifier
            int32_t c = lexer->lookahead;
            bool is_boundary = (c == 0 || c == ' ' || c == '\t' || c == '\n' ||
                                 c == '\r' || c == '(' || c == ')' || c == '{' ||
                                 c == '}' || c == ';' || c == '"' || c == '@');
            if (is_boundary) {
                lexer->mark_end(lexer);  // AS_RENAME spans just 'a','s'
                // Skip horizontal whitespace
                while (c == ' ' || c == '\t') {
                    advance(lexer);
                    c = lexer->lookahead;
                }
                // Consume module-name (including '.' for qualified names)
                while (c != 0 && c != ' ' && c != '\t' && c != '\n' &&
                       c != '\r' && c != '(' && c != ')' && c != '{' &&
                       c != '}' && c != ';' && c != '"' && c != '@') {
                    advance(lexer);
                    c = lexer->lookahead;
                }
                // Skip whitespace after module-name
                while (c == ' ' || c == '\t') {
                    advance(lexer);
                    c = lexer->lookahead;
                }
                // Check whether what follows is another `as` word
                if (c == 'a') {
                    advance(lexer);
                    if (lexer->lookahead == 's') {
                        advance(lexer);
                        c = lexer->lookahead;
                        bool next_bound = (c == 0 || c == ' ' || c == '\t' ||
                                           c == '\n' || c == '\r' || c == '(' ||
                                           c == ')' || c == '{' || c == '}' ||
                                           c == ';' || c == '"' || c == '@');
                        if (next_bound) {
                            // Module-name is followed by another `as` → this `as`
                            // is better parsed as an identifier atom; do not emit.
                            return false;
                        }
                    }
                    // 'a' + non-'s': next is not `as`, fall through to emit
                }
                // Next after module-name is not `as` → emit AS_RENAME
                lexer->result_symbol = AS_RENAME;
                return true;
            }
        }
        // Not `as` or not a word boundary → fall through
        return false;
    }

    // Handle _LAMBDA before layout processing.  The external scanner is called
    // with the raw character stream (including leading spaces), so we skip
    // horizontal whitespace first.
    //
    // '\' rules: '\' followed by an ASCII letter or whitespace is always LAMBDA
    // (because the id regex excludes letters after '\', so '\letter' can never
    // be a single identifier token).  '\' followed by '{' or '(' is also LAMBDA
    // (lambda with implicit binding or typed/absurd binding, e.g. \{x}, \(x:T),
    // \()).  '\' followed by anything else (e.g. '/') is NOT lambda — the
    // built-in lexer then matches the multi-char operator as sym_id (e.g. \/).
    //
    // 'λ' (U+03BB) rules: 'λ' followed by whitespace, '{', or '(' is LAMBDA.
    // 'λ' followed by a letter or '-' is NOT lambda — it forms an identifier
    // such as 'λ-elim' or 'λx→x' (common in Agda standard library).  The id
    // regex's first alternative matches 'λ' as a valid id start, and the
    // continuation chars include letters and '-', so 'λelim', 'λ-elim', etc.
    // are all parsed as single identifier tokens by the built-in lexer.
    //
    // Returning false resets the lexer position so the built-in lexer runs.
    // The LAMBDA check is an error-recovery aid: it fires only when no layout
    // token (NEWLINE/INDENT/DEDENT/LAMBDA_NEWLINE/BLOCK_COMMENT) is expected.
    // We do NOT test valid_symbols[LAMBDA] because LAMBDA == num_external_tokens
    // (5), which is one past the end of valid_symbols — reading it is UB.
    // The negative checks on the five real externals are sufficient: the scanner
    // is only called when at least one external is valid, so if all five are
    // false we are in a pure error-recovery context where emitting LAMBDA is safe.
    if (!valid_symbols[NEWLINE] && !valid_symbols[INDENT] && !valid_symbols[DEDENT] &&
        !valid_symbols[LAMBDA_NEWLINE] && !valid_symbols[BLOCK_COMMENT] &&
        !valid_symbols[OPEN_IDIOM] && !valid_symbols[EMPTY_IDIOM] &&
        !valid_symbols[AS_RENAME]) {
        // Skip horizontal whitespace before checking the lambda character.
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            skip(lexer);
        }
        if (lexer->lookahead == '\\') {
            advance(lexer);
            uint32_t next = lexer->lookahead;
            // '\' is lambda when followed by: whitespace, letter, '{', '('
            // '\' is NOT lambda when followed by operator chars like '/', '_', etc.
            bool valid_after_bslash =
                next == 0 || next == '\n' || next == '\r' ||
                next == ' ' || next == '\t' ||
                (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                next == '{' || next == '(';
            if (valid_after_bslash) {
                lexer->result_symbol = LAMBDA;
                return true;
            }
            return false;
        }
        if (lexer->lookahead == 0x03BB) {  // λ
            advance(lexer);
            uint32_t next = lexer->lookahead;
            // 'λ' is lambda when followed by: whitespace, '{', '('
            // 'λ' is NOT lambda when followed by a letter or operator char:
            // 'λelim', 'λ-elim', 'λx→x' are identifiers, not lambda expressions.
            bool valid_after_lambda =
                next == 0 || next == '\n' || next == '\r' ||
                next == ' ' || next == '\t' ||
                next == '{' || next == '(';
            if (valid_after_lambda) {
                lexer->result_symbol = LAMBDA;
                return true;
            }
            return false;
        }
    }

    if (QUEUE_EMPTY(scanner->tokens)) {
        {
            bool skipped_newline = false;

            while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
                   lexer->lookahead == '\r' || lexer->lookahead == '\n') {
                if (lexer->lookahead == '\n') {
                    skipped_newline = true;
                    skip(lexer);
                } else {
                    skip(lexer);
                }
            }
            if (lexer->eof(lexer)) {
                // At EOF, flush any queued DEDENT before handling stack-based DEDENT.
                if (valid_symbols[DEDENT] && scanner->queued_dedent_count > 0) {
                    scanner->queued_dedent_count--;
                    QUEUE_PUSH(scanner->tokens, DEDENT);
                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                } else if (valid_symbols[DEDENT] && scanner->indents.len > 1) {
                    // Pop a still-open layout block at EOF (e.g. `let\n  x = y\nin P Q`
                    // at end of file): pop the inline entry and emit DEDENT so the
                    // let rule closes cleanly.
                    VEC_POP(scanner->indents);
                    QUEUE_PUSH(scanner->tokens, DEDENT);
                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                } else if (valid_symbols[NEWLINE]) {
                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                }
            } else {
                bool next_token_is_comment = false;
                // Set when the whitespace BLOCK_COMMENT check (below) already
                // called mark_end at '{' and advanced past it.  Guards the
                // INDENT handler's else-if('{') branch so it does not fire a
                // second time for the next character — which would overwrite
                // mark_end with the wrong position.  Triggered for any '{X'
                // sequence (records, pragmas, double-braces …); the INDENT
                // handler's own peek is only needed when that check did not run.
                bool brace_peeked = false;
                // Set when the _open_idiom check below has already called
                // mark_end at '(' and advanced past it.  The same-level NEWLINE
                // comment check must not call mark_end again at '-' in that
                // case — doing so would move the NEWLINE token boundary past '(',
                // causing '(' to be consumed by the NEWLINE token.  For example,
                // `(-[1+ n ]) ×ᶻ x = …` at the start of a line: the idiom peek
                // advances to '-', then the comment check finds '-' and calls
                // mark_end there, turning NEWLINE into a 2-byte token that eats
                // '('.  The function LHS then starts at col 1 ('-') instead of
                // col 0 ('('), leaving ')' at col 9 unmatched → ERROR.
                bool open_paren_peeked = false;

                uint16_t indent_length = (uint16_t)lexer->get_column(lexer);
                uint16_t top_column = INDENT_COLUMN(VEC_BACK(scanner->indents));

                bool indent = indent_length > top_column;
                bool dedent = indent_length < top_column;

                // When the next token is a block comment AND no DEDENT is needed
                // at the current column, emit it as BLOCK_COMMENT.  This handles
                // cases where a block comment appears after a leading newline that
                // was skipped above (e.g. a file starting with a blank line then
                // `{- ... -}`).  When `dedent` is true the enclosing block must
                // be closed (DEDENT emitted) before the comment is returned; in
                // that case fall through to the layout logic below.
                // Use mark_end + peek-only advances so non-block-comment `{` cases
                // (records, pragmas) fall through to the normal layout processing.
                if (valid_symbols[BLOCK_COMMENT] && lexer->lookahead == '{' && !dedent) {
                    lexer->mark_end(lexer);           // save position at {
                    lexer->advance(lexer, false);     // peek past {
                    brace_peeked = true;              // mark_end already set at '{'; INDENT handler must not re-peek
                    if (lexer->lookahead == '-') {
                        lexer->advance(lexer, false); // peek past -
                        if (lexer->lookahead != '#') {
                            // Confirmed block comment, no pending DEDENT: scan and emit.
                            scan_block_comment_body(lexer);
                            lexer->mark_end(lexer);   // update end to after closing }
                            lexer->result_symbol = BLOCK_COMMENT;
                            return true;
                        }
                        // {-# pragma: fall through with mark_end at {
                    }
                    // {X (not {-): fall through with mark_end at {
                    // Layout processing continues; mark_end at { means any emitted
                    // zero-width layout token ends just before {, resuming there.
                }

                // Handle OPEN_IDIOM / EMPTY_IDIOM after whitespace.  Mirrors the
                // BLOCK_COMMENT check above: only fire when no DEDENT is needed.
                // `(|` followed by non-id is OPEN_IDIOM; `(|)` is EMPTY_IDIOM.
                if ((valid_symbols[OPEN_IDIOM] || valid_symbols[EMPTY_IDIOM]) &&
                    lexer->lookahead == '(' && !dedent) {
                    lexer->mark_end(lexer);           // save position at (
                    lexer->advance(lexer, false);     // peek past (
                    if (lexer->lookahead == '|') {
                        lexer->advance(lexer, false); // peek past |
                        if (lexer->lookahead == ')' && valid_symbols[EMPTY_IDIOM]) {
                            lexer->advance(lexer, false); // peek past )
                            lexer->mark_end(lexer);
                            lexer->result_symbol = EMPTY_IDIOM;
                            return true;
                        }
                        if (valid_symbols[OPEN_IDIOM]) {
                            int32_t c = lexer->lookahead;
                            bool next_is_id = (c != 0 && c != '\n' && c != '\r' &&
                                               c != ' ' && c != '\t' &&
                                               c != ';' && c != '.' && c != '"' &&
                                               c != '(' && c != ')' &&
                                               c != '{' && c != '}' && c != '@');
                            if (!next_is_id) {
                                lexer->mark_end(lexer);
                                lexer->result_symbol = OPEN_IDIOM;
                                return true;
                            }
                        }
                    }
                    // Not (| or not idiom context — fall through to layout.
                    // mark_end at ( means any emitted zero-width layout token
                    // ends just before (, resuming there.  Record that we
                    // already advanced past '(' so the NEWLINE comment check
                    // below does not overwrite mark_end at '-'.
                    open_paren_peeked = true;
                }

                // Handle AS_RENAME after whitespace — same logic as the pre-whitespace
                // check above but fires here when `as` is reached after consuming
                // leading spaces/newlines.  Must run before the DEDENT check so it
                // is not skipped by the layout machinery when !dedent.
                // Suppress when a newline was crossed: `abstract` or other
                // a-leading declarations after `import M` must not be consumed
                // as AS_RENAME — the layout machinery should emit NEWLINE first.
                if (valid_symbols[AS_RENAME] && lexer->lookahead == 'a' && !dedent &&
                    !skipped_newline) {
                    advance(lexer);  // consume 'a'
                    if (lexer->lookahead == 's') {
                        advance(lexer);  // consume 's'
                        int32_t c = lexer->lookahead;
                        bool is_boundary = (c == 0 || c == ' ' || c == '\t' ||
                                             c == '\n' || c == '\r' || c == '(' ||
                                             c == ')' || c == '{' || c == '}' ||
                                             c == ';' || c == '"' || c == '@');
                        if (is_boundary) {
                            lexer->mark_end(lexer);
                            while (c == ' ' || c == '\t') {
                                advance(lexer);
                                c = lexer->lookahead;
                            }
                            // Consume module-name
                            while (c != 0 && c != ' ' && c != '\t' && c != '\n' &&
                                   c != '\r' && c != '(' && c != ')' && c != '{' &&
                                   c != '}' && c != ';' && c != '"' && c != '@') {
                                advance(lexer);
                                c = lexer->lookahead;
                            }
                            // Skip whitespace after module-name
                            while (c == ' ' || c == '\t') {
                                advance(lexer);
                                c = lexer->lookahead;
                            }
                            // If followed by another `as` word, do not emit
                            if (c == 'a') {
                                advance(lexer);
                                if (lexer->lookahead == 's') {
                                    advance(lexer);
                                    c = lexer->lookahead;
                                    bool nb = (c == 0 || c == ' ' || c == '\t' ||
                                               c == '\n' || c == '\r' || c == '(' ||
                                               c == ')' || c == '{' || c == '}' ||
                                               c == ';' || c == '"' || c == '@');
                                    if (nb) return false;
                                }
                                // 'a' + non-'s': not `as`, proceed to emit
                            }
                            lexer->result_symbol = AS_RENAME;
                            return true;
                        }
                    }
                    return false;
                }

                // Fire a queued DEDENT only when the next token is at or below
                // the current stack top AND the current stack top is NOT an inline
                // entry.  Firing while indent_length > top_column (inside a deeper
                // block) would close the block too early — e.g. in `let … in P\nQ`
                // the queued DEDENT for the `let` INDENT must not fire at Q while
                // the in-body expression is still being parsed.  The !INLINE guard
                // prevents a stale qdc (set when an outer inline block was popped
                // via the non-condition-746 path on a GLR branch where NEWLINE was
                // not valid) from firing inside a subsequently-opened inner inline
                // block — e.g. after `f = let x = k in do y ← g\n  h`, a qdc=1
                // for the let-block persists on stale GLR paths; it must not fire at
                // the same column as `do`'s inline-INDENT (which would prematurely
                // close the do-block after its first stmt).
                //
                // Drain queued inline-`let` closures from the non-inline-top
                // while loop, emitting DEDENT when the grammar accepts it.  When
                // DEDENT is not valid at the first fresh call after the paired
                // DEDENT+NEWLINE, the inline let's close was already consumed as
                // absent in this GLR branch (zombie entry) — discard silently so
                // no spurious DEDENT closes an enclosing block.
                if (scanner->queued_inline_let_count > 0 && !indent &&
                    !INDENT_IS_INLINE(VEC_BACK(scanner->indents))) {
                    if (valid_symbols[DEDENT]) {
                        scanner->queued_inline_let_count--;
                        QUEUE_PUSH(scanner->tokens, DEDENT);
                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                        goto done_enqueue;
                    } else {
                        // DEDENT not valid: inline let was a zombie — discard.
                        scanner->queued_inline_let_count = 0;
                    }
                }
                if (scanner->queued_inline_where_count > 0 && !indent &&
                    !INDENT_IS_INLINE(VEC_BACK(scanner->indents))) {
                    if (valid_symbols[DEDENT]) {
                        scanner->queued_inline_where_count--;
                        QUEUE_PUSH(scanner->tokens, DEDENT);
                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                        goto done_enqueue;
                    }
                    // D=0: grammar not yet ready to close inline where-block;
                    // do NOT zombie-discard — keep qiwc alive and retry at next call.
                }
                if (scanner->queued_inline_do_count > 0 && !indent &&
                    !INDENT_IS_INLINE(VEC_BACK(scanner->indents))) {
                    // Close a pending inline do-block.  Its last statement may be
                    // a multi-line `λ where` whose lambda-application has not yet
                    // reduced, so DEDENT (the do-block close) is not valid until
                    // the do-statement reduces.
                    if (valid_symbols[DEDENT]) {
                        scanner->queued_inline_do_count--;
                        QUEUE_PUSH(scanner->tokens, DEDENT);
                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                        goto done_enqueue;
                    } else if (valid_symbols[NEWLINE]) {
                        // DEDENT not yet valid but NEWLINE is: emit a NEWLINE (the
                        // do-statement separator) to force the trailing
                        // application to reduce; keep qidc and retry — DEDENT will
                        // be valid on the next call.
                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                        goto done_enqueue;
                    }
                    // Neither valid yet: keep qidc alive and retry (never discard).
                }
                if (valid_symbols[DEDENT] &&
                    scanner->queued_dedent_count > 0 && !indent &&
                    !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                    // Only drain when we are strictly below the minimum column
                    // of the qdc'd block, preventing a spurious DEDENT from
                    // firing inside a nested block at a higher column (e.g.
                    // a `private` block at col 4 when the qdc is for a module-
                    // body block at col 2).  The sentinel 0xFFFF means
                    // unconstrained (qdc was not set by the column-tracked path).
                    (scanner->queued_dedent_min_col == 0xFFFF ||
                     (uint16_t)indent_length < scanner->queued_dedent_min_col)) {
                    scanner->queued_dedent_count--;
                    if (scanner->queued_dedent_count == 0) {
                        scanner->queued_dedent_min_col = 0xFFFF;
                    }
                    QUEUE_PUSH(scanner->tokens, DEDENT);
                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                    goto done_enqueue;
                }
                if (!next_token_is_comment) {
                    if (skipped_newline) {
                        // Crossing a newline ends any same-line spurious-push
                        // suppression that was set by a cross-newline inline DEDENT.
                        scanner->after_cross_newline_inline_dedent = false;
                        if (indent) {
                            // A `where` keyword on a new line indented DEEPER than
                            // an enclosing `λ where` clause block does NOT continue
                            // the last clause (lambda clauses have no where-clause):
                            // it is the enclosing function's where-clause.  Close
                            // the λ-where block with a DEDENT so the grammar can
                            // attach the `where` at the function level.  (Agda does
                            // this via `close = error {% popBlock }`; here we
                            // recognise the block by LAM_WHERE_FLAG.)  Only fires
                            // when the block's own `_dedent` close is valid.
                            if (lexer->lookahead == 'w' &&
                                scanner->indents.len > 1 &&
                                INDENT_IS_LAM_WHERE(VEC_BACK(scanner->indents)) &&
                                valid_symbols[DEDENT] &&
                                peek_word_keyword(lexer, "where")) {
                                VEC_POP(scanner->indents);
                                QUEUE_PUSH(scanner->tokens, DEDENT);
                                goto done_enqueue;
                            }
                            // When INDENT is not expected (expression context,
                            // e.g. after `=` in a function clause), check
                            // whether the indented content starts with a lambda
                            // character.  Without this check, the external
                            // scanner would return false (no INDENT to emit),
                            // and the built-in lexer would match `λ`/`\` as a
                            // plain identifier token.  This handles patterns
                            // like `f x =\n  λ y → y` and `f x =\n  \ y → y`.
                            // Do NOT emit LAMBDA here for `\` or `λ` at an
                            // indented position after a newline.  Error-recovery
                            // GLR stacks have valid_symbols[LAMBDA]=true; emitting
                            // LAMBDA would consume the `λ` character and prevent
                            // the good stacks from seeing it as the lambda keyword
                            // via the built-in lexer.  Returning false lets the
                            // built-in lexer match `λ` as the keyword token in
                            // patterns like `f x =\n  λ y → y`.
                            if (valid_symbols[INDENT]) {
                                // If the indented content is a `--` or `{-` comment,
                                // do not push INDENT.  The comment will be consumed as
                                // an extra by the built-in lexer; the scanner is called
                                // again and either pushes a real INDENT from the next
                                // non-comment line's column or handles same-level/dedent.
                                //
                                // When the lookahead is `{` or `-`, peek ahead to
                                // distinguish `{-`/`--` comments from real content
                                // (`{id}` implicit patterns, operators starting with `-`).
                                // Call mark_end INSIDE each branch BEFORE advancing so
                                // the INDENT token ends at the character boundary and
                                // does NOT include `{` or `-` in its span.
                                //
                                // Note: when valid_symbols[BLOCK_COMMENT] is true, the
                                // earlier check at line 600 already called mark_end at
                                // `{` and advanced past it — lookahead is then already
                                // past `{`, so the `else if ('{')` branch below does not
                                // re-fire and the mark from line 600 is preserved.
                                if (lexer->lookahead == '-') {
                                    lexer->mark_end(lexer);
                                    advance(lexer);
                                    if (lexer->lookahead == '-') {
                                        return false;
                                    }
                                } else if (lexer->lookahead == '{' && !brace_peeked) {
                                    // Only enter when brace_peeked is false: the
                                    // whitespace BLOCK_COMMENT check above already
                                    // called mark_end at '{' and advanced past it.
                                    // For '{{', that leaves lookahead at the second
                                    // '{'; firing here again would call mark_end at
                                    // the second '{', consuming the first one inside
                                    // the INDENT span and breaking '{{' lexing.
                                    lexer->mark_end(lexer);
                                    advance(lexer);
                                    if (lexer->lookahead == '-') {
                                        advance(lexer);
                                        if (lexer->lookahead != '#' && valid_symbols[BLOCK_COMMENT]) {
                                            scan_block_comment_body(lexer);
                                            lexer->mark_end(lexer);
                                            lexer->result_symbol = BLOCK_COMMENT;
                                            return true;
                                        } else if (lexer->lookahead != '#') {
                                            return false;
                                        }
                                    }
                                }
                                // Set PAREN_FLAG when NEWLINE is not valid at
                                // the push site — that indicates we're inside a
                                // parenthesised expression (e.g. `(do …)`) as
                                // opposed to a module-level block boundary where
                                // NEWLINE is always valid.  The `)` handler uses
                                // this flag to distinguish a do-block that must
                                // close before `)` from an outer module block
                                // that should never be closed by `)`.
                                uint32_t entry = valid_symbols[NEWLINE]
                                    ? INDENT_MAKE(indent_length, 0)
                                    : INDENT_MAKE_PAREN(indent_length);
                                // Tag a `let`'s declarations block (valid_symbols
                                // [LET_INDENT] is true only right after `let`), even
                                // when it is opened across a newline (multi-line
                                // `let\n  x = …`), so the `in`-handler can close it.
                                if (valid_symbols[LET_INDENT]) {
                                    entry |= LET_INLINE_FLAG;
                                }
                                // Tag a do-block's INDENT (valid_symbols[DO_INDENT]
                                // is true only at a `do`-block opener) so the `)`
                                // handler can cascade-close it.
                                if (valid_symbols[DO_INDENT]) {
                                    entry |= DO_BLOCK_FLAG;
                                }
                                // Tag a `λ where` clause block's INDENT so a
                                // deeper-indented `where` keyword can close it.
                                if (valid_symbols[LAM_WHERE_INDENT]) {
                                    entry |= LAM_WHERE_FLAG;
                                }
                                VEC_PUSH(scanner->indents, entry);
                                QUEUE_PUSH(scanner->tokens, INDENT);
                            }
                        } else if (dedent) {
                            // Track whether the '{X' peek below already called
                            // mark_end at '{' and advanced past it.  Used below to
                            // prevent a second mark_end (at 'X', after '{') from
                            // overwriting the correct '{' boundary inside the INDENT
                            // path (e.g. `{Chain}` after `let open A in`).
                            bool brace_peeked_in_dedent = false;
                            // A `--` line comment at column 0 must not close the
                            // current layout block.  Column-0 comments inside a
                            // module or where-block are almost always commented-out
                            // code; emitting DEDENT here would close the block and
                            // strand whatever follows.  Return false so the
                            // built-in lexer consumes the comment as an extra; the
                            // scanner is reinvoked at the actual next token.
                            //
                            // We restrict this to indent_length == 0 so that
                            // comments at positive columns (e.g. col 4 between two
                            // `instance` blocks in a record) still trigger DEDENT
                            // before being consumed, preserving their sibling
                            // placement in the parse tree.
                            //
                            // Block comment `{- … -}` at a lower column is
                            // always skipped (original behaviour).
                            //
                            // We also skip (defer) a `--` comment at a positive
                            // column when we are inside an INLINE `λ where` clause
                            // context (LAMBDA_NEWLINE valid, NEWLINE not, and the
                            // top layout entry is an inline INDENT).  A comment is
                            // transparent to layout, so the clause-separator vs
                            // close decision must be made from the next REAL token,
                            // not the comment line.  Without this, a comment
                            // indented below an inline `λ where` clause (e.g.
                            //   inj : A
                            //     → Σ B λ where refl → z
                            //     -- a comment
                            //   inj refl = d
                            // ) is mis-read as a new λ-where clause: LAMBDA_NEWLINE
                            // is emitted for the comment, leaving the grammar stuck
                            // in the λ-where state and corrupting a later clause.
                            //
                            // Restricted to an INLINE top: a non-inline `λ where`
                            // block (clauses on their own lines) relies on the
                            // comment's column to trigger the right DEDENT cascade
                            // (see the "Same-line inline where with multiline
                            // lambda-where followed by comment" corpus test), so
                            // we must NOT defer it there.
                            if (lexer->lookahead == '-' &&
                                (indent_length == 0 ||
                                 (valid_symbols[LAMBDA_NEWLINE] &&
                                  !valid_symbols[NEWLINE] &&
                                  scanner->indents.len > 1 &&
                                  INDENT_IS_INLINE(VEC_BACK(scanner->indents))))) {
                                // Distinguish `--` line comment from identifier
                                // starting with `-` (e.g. `-raw`).  mark_end at
                                // `-` first so that if it turns out to be an id
                                // (not `--`), the lexer position resets to `-`
                                // and the built-in lexer can lex `-raw` as a
                                // single identifier.
                                lexer->mark_end(lexer);
                                advance(lexer);
                                if (lexer->lookahead == '-') {
                                    // `--` line comment: return false so the
                                    // built-in lexer consumes it as an extra via
                                    // the inline comment regex; the scanner is
                                    // reinvoked at the actual next token for
                                    // DEDENT/same-level handling.
                                    return false;
                                }
                                // Not a comment — fall through to the layout
                                // logic below (DEDENT for col-0 content).  The
                                // mark_end above ensures any emitted layout
                                // token ends just before `-`, so the built-in
                                // lexer resumes at `-` and lexes `-raw` as id.
                            } else if (lexer->lookahead == '{') {
                                // block comment or pragma: `{-` or `{-#`
                                // mark_end at `{` so any zero-width layout token
                                // (DEDENT) emitted after fall-through ends here
                                // and the next scan call resumes at `{`.
                                lexer->mark_end(lexer);
                                advance(lexer);
                                if (lexer->lookahead == '-') {
                                    advance(lexer);
                                    if (lexer->lookahead != '#' && valid_symbols[BLOCK_COMMENT]) {
                                        // Block comment in dedent position: emit it
                                        // so the layout block stays open (comments
                                        // are transparent to Agda's layout rules).
                                        scan_block_comment_body(lexer);
                                        lexer->mark_end(lexer);
                                        lexer->result_symbol = BLOCK_COMMENT;
                                        return true;
                                    } else if (lexer->lookahead != '#') {
                                        return false;
                                    }
                                    // {-# pragma: fall through; DEDENT will end at {
                                    brace_peeked_in_dedent = true;
                                }
                                // {X (not {-): fall through; DEDENT ends at {
                                brace_peeked_in_dedent = true;
                            }
                            // When the top is an inline entry (pushed mid-line,
                            // e.g. by `let` in `let x = y in z`) and the next
                            // line dedents below it, we need to decide whether to
                            // close the inline block (DEDENT) or start a new real
                            // block (INDENT).  DEDENT takes priority when both
                            // DEDENT and NEWLINE are valid (we're at a layout
                            // level — function body, source_file — where the let
                            // can close and a new declaration can follow).  The
                            // NEWLINE guard is required: if NEWLINE is not valid
                            // (e.g., inside `record { … }` where `;` not NEWLINE
                            // is the separator), the let body expression has not
                            // yet finished (e.g. `let open M in λ where { … }`),
                            // and emitting DEDENT here would close the let block
                            // before the body is parsed.  Fall through to the
                            // INDENT path when DEDENT is not valid (the `X where
                            // module M … where` case).
                            if (scanner->indents.len > 1 &&
                                INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                valid_symbols[DEDENT] &&
                                valid_symbols[NEWLINE]) {
                                VEC_POP(scanner->indents);
                                // After popping the inline entry, check the new
                                // outer top.  If indent_length == new outer top we
                                // are at the SAME LEVEL as the outer block (e.g.
                                // the module body at col 2).  In GLR, emitting
                                // DEDENT here is ambiguous: it would be consumed
                                // both by optional($._dedent) in the `let` rule AND
                                // by the outer block's $._dedent (closing the module
                                // prematurely before h1 is parsed).  Emit NEWLINE
                                // only; optional($._dedent) simply skips, and the
                                // module body's declaration separator gets NEWLINE.
                                // Only emit DEDENT when indent_length < new outer
                                // top (we're genuinely dedenting below the outer
                                // block — e.g. a `let` inside a where-clause that
                                // is followed by a top-level declaration).
                                uint16_t new_outer_top =
                                    INDENT_COLUMN(VEC_BACK(scanner->indents));
                                // At the same level (see above), suppress DEDENT
                                // (emit NEWLINE only) when the outer block is a
                                // regular layout block (NEWLINE was
                                // valid at INDENT push time, i.e. !PAREN_FLAG).
                                // Module bodies, where-clauses, and source_file
                                // blocks are all opened when NEWLINE is valid.
                                //
                                // We keep emitting DEDENT when the outer block is a
                                // paren/expression block (PAREN_FLAG set — NEWLINE
                                // was NOT valid at push time, e.g. `do` inside `f =
                                // do`).  In that context let_in_do's
                                // seq($._newline, $._dedent) genuinely needs DEDENT.
                                bool outer_is_paren =
                                    INDENT_IS_PAREN(VEC_BACK(scanner->indents));
                                if (indent_length < new_outer_top ||
                                    outer_is_paren) {
                                    // Mark that we just closed an inline block via a
                                    // cross-newline dedent (e.g. `(let b = x\n    )`).
                                    // This suppresses spurious same-line inline INDENT
                                    // pushes on the tokens that follow `)`.
                                    // Only set for lambda-where entries (DEDENT not
                                    // valid), not for let entries — let's optional tail
                                    // keeps DEDENT valid, and setting this flag for
                                    // let entries prevents subsequent `let`s in sibling
                                    // record fields from pushing their INDENT entries.
                                    if (!valid_symbols[DEDENT]) {
                                        scanner->after_cross_newline_inline_dedent = true;
                                    }
                                    // When genuinely dedenting BELOW the outer block
                                    // (not a paren block), pop the outer entry and
                                    // queue its DEDENT via queued_dedent_count so the
                                    // grammar receives the token the next time DEDENT
                                    // is valid.  Also pop any additional enclosing
                                    // entries that are now above indent_length, each
                                    // queuing its own DEDENT.
                                    if (indent_length < new_outer_top && !outer_is_paren) {
                                        // Pop the outer layout block (e.g. instance or
                                        // private) and queue its DEDENT.  Without the
                                        // qdc++ the grammar never receives the DEDENT
                                        // needed to close that block, causing ERROR when
                                        // an outer-scope declaration follows an inline
                                        // `λ where` or `let…in` inside the block.
                                        // When popping additional INLINE entries (e.g.
                                        // nested let_inline chains in `let α = a in let β
                                        // = b in body\n  next`), route through
                                        // queued_inline_let_count so each is closed by
                                        // DEDENT (absorbed by the let rule's
                                        // optional($._dedent)) rather than DEDENT.
                                        // Plain qdc++ would emit DEDENT, breaking the
                                        // outer let's body continuation.
                                        // Distinguish nested let_inline chains (route
                                        // through queued_inline_let_count → DEDENT)
                                        // from record_where/lambda_where inlines (route
                                        // through queued_dedent_count → DEDENT).  let_inline
                                        // is INLINE without DECL_INLINE_FLAG (NEWLINE not
                                        // valid at push time, mid-expression); record_where
                                        // and similar declaration-context inlines have
                                        // DECL_INLINE_FLAG set.
                                        bool _po_is_let_like =
                                            INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                            !INDENT_IS_DECL_INLINE(VEC_BACK(scanner->indents));
                                        VEC_POP(scanner->indents);
                                        if (_po_is_let_like && valid_symbols[DEDENT]) {
                                            scanner->queued_inline_let_count++;
                                        } else {
                                            scanner->queued_dedent_count++;
                                        }
                                        while (indent_length <
                                               INDENT_COLUMN(VEC_BACK(scanner->indents))) {
                                            bool _po_is_let_like_w =
                                                INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                                !INDENT_IS_DECL_INLINE(VEC_BACK(scanner->indents));
                                            VEC_POP(scanner->indents);
                                            if (_po_is_let_like_w &&
                                                valid_symbols[DEDENT]) {
                                                scanner->queued_inline_let_count++;
                                            } else {
                                                scanner->queued_dedent_count++;
                                            }
                                        }
                                    }
                                    // Close the inline let entry with the unified
                                    // DEDENT.
                                    QUEUE_PUSH(scanner->tokens, DEDENT);
                                    // Do NOT emit a trailing NEWLINE when `where`
                                    // immediately follows the dedented position.
                                    if (valid_symbols[NEWLINE]) {
                                        bool _next_is_where =
                                            (lexer->lookahead == 'w') &&
                                            peek_word_keyword(lexer, "where");
                                        bool _next_is_rparen = (lexer->lookahead == ')');
                                        if (!_next_is_where && !_next_is_rparen) {
                                            QUEUE_PUSH(scanner->tokens, NEWLINE);
                                        }
                                    }
                                } else {
                                    // Same-level or above outer block: the inline
                                    // entry was at a deeper/equal column than the
                                    // outer block.  Check what follows.
                                    //
                                    // When `where` follows: emit DEDENT (not
                                    // NEWLINE).  This commits the VEC_POP above so
                                    // that tree-sitter saves the new state [col_0]
                                    // rather than discarding it on `return false`.
                                    // Without this, the stale scanner state
                                    // ([col_0, col_12_INLINE]) survives across GLR
                                    // stack merging and later emits a spurious
                                    // DEDENT+NEWLINE at the `where` position of the
                                    // NEXT function, stranding that function's
                                    // where-clause as a phantom declaration.
                                    //
                                    // For any other token: emit NEWLINE as a
                                    // declaration separator (matching the old
                                    // same-level behaviour that worked before
                                    // `where`-specific suppression was added).
                                    bool _next_is_rparen = (lexer->lookahead == ')');
                                    bool _next_is_where =
                                        (lexer->lookahead == 'w') &&
                                        peek_word_keyword(lexer, "where");
                                    if (_next_is_where) {
                                        // Emit DEDENT (no NEWLINE) — commits the
                                        // inline-block pop and lets the grammar
                                        // match `where` via optional($.where).
                                        // Emitting NEWLINE here instead would
                                        // strand the `where` as a phantom
                                        // declaration.  Set the suppress flag only
                                        // for lambda entries (DEDENT not valid at
                                        // this point), not let entries.
                                        if (!valid_symbols[DEDENT]) {
                                            scanner->after_cross_newline_inline_dedent = true;
                                        }
                                        QUEUE_PUSH(scanner->tokens, DEDENT);
                                    } else if (!_next_is_rparen) {
                                        // If we're above the outer block
                                        // (indent_length > new_outer_top), the let
                                        // body expression is still in progress: a
                                        // multi-line application like
                                        //   let module SR = S X in
                                        //   SR.b
                                        //      SR.c
                                        // where SR.c continues SR.b at a deeper column.
                                        // Return false (queue empty → done_enqueue
                                        // returns false) so GLR can parse SR.c as
                                        // another atom in the let body.  The INLINE
                                        // pop is rolled back; DEDENT will be
                                        // emitted when the body truly ends (same-level
                                        // or at EOF via the EOF handler).
                                        //
                                        // When indent_length == new_outer_top (same
                                        // level as outer block), the let body is
                                        // genuinely done — emit DEDENT so the let
                                        // closes and the outer block separator follows.
                                        //
                                        // NEW (close-token unification): if the next
                                        // token is the `in` keyword, the let block's
                                        // close must be emitted HERE (Agda's `vclose`
                                        // before the body) so the grammar's
                                        // `optional($._dedent)` that precedes
                                        // `$._let_body` consumes it.  Emitting DEDENT
                                        // also commits the VEC_POP above (returning
                                        // false would roll it back, leaving the let
                                        // block stale and breaking later closes —
                                        // e.g. `foo = let a = e in k a` followed by a
                                        // sibling decl / module).  No NEWLINE: `in`
                                        // follows the close directly.
                                        bool _next_is_in =
                                            (lexer->lookahead == 'i') &&
                                            peek_word_keyword(lexer, "in");
                                        if (_next_is_in && valid_symbols[DEDENT]) {
                                            QUEUE_PUSH(scanner->tokens, DEDENT);
                                        } else if (valid_symbols[DEDENT] &&
                                            indent_length > new_outer_top) {
                                            // leave queue empty → goto done_enqueue
                                            // returns false below
                                        } else if (valid_symbols[DEDENT]) {
                                            QUEUE_PUSH(scanner->tokens, DEDENT);
                                            if (valid_symbols[NEWLINE]) {
                                                QUEUE_PUSH(scanner->tokens, NEWLINE);
                                            }
                                        } else if (valid_symbols[LAMBDA_NEWLINE]) {
                                            // Same logic as the top_was_inline branch
                                            // below: `;`, `}`, or `where` closes the
                                            // inline lambda-where block.
                                            bool _lnl2_closes =
                                                (lexer->lookahead == ';' ||
                                                 lexer->lookahead == '}') ||
                                                ((lexer->lookahead == 'w') &&
                                                 peek_word_keyword(lexer, "where"));
                                            if (_lnl2_closes && valid_symbols[DEDENT]) {
                                                QUEUE_PUSH(scanner->tokens, DEDENT);
                                            } else {
                                                QUEUE_PUSH(scanner->tokens, LAMBDA_NEWLINE);
                                            }
                                        } else if (valid_symbols[NEWLINE]) {
                                            QUEUE_PUSH(scanner->tokens, NEWLINE);
                                        }
                                    }
                                }
                            } else if (scanner->indents.len > 1 &&
                                INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                valid_symbols[INDENT]) {
                                // Walk past all consecutive inline entries (e.g.
                                // `let_inline` below `outer_do_inline` in
                                // `let x = do y do\n  h`).  Without this, the
                                // outer bound would be a stale inline column,
                                // blocking the INDENT push for the inner do at
                                // a column below all inline entries but above
                                // the base non-inline block.
                                int _oi = (int)scanner->indents.len - 2;
                                while (_oi > 0 &&
                                       INDENT_IS_INLINE(
                                           scanner->indents.data[_oi])) {
                                    _oi--;
                                }
                                uint16_t outer =
                                    INDENT_COLUMN(scanner->indents.data[_oi]);
                                if (indent_length > outer) {
                                    {
                                        // Mark the token end at the current lexer
                                        // position (start of the body token, after
                                        // the newline/whitespace).  Without this,
                                        // the INDENT is zero-width and the next
                                        // scanner call re-skips the same newline,
                                        // triggering a spurious same-level NEWLINE
                                        // at the first token of the let body.
                                        // Do NOT call mark_end when the '{X' peek
                                        // above already set mark_end at '{' — calling
                                        // it again would move the boundary past '{'
                                        // and include '{' in the INDENT token span.
                                        if (!brace_peeked_in_dedent) {
                                            lexer->mark_end(lexer);
                                        }
                                        uint32_t entry = valid_symbols[NEWLINE]
                                            ? INDENT_MAKE(indent_length, 0)
                                            : INDENT_MAKE_CONVERTED(indent_length);
                                        VEC_PUSH(scanner->indents, entry);
                                        QUEUE_PUSH(scanner->tokens, INDENT);
                                    }
                                } else {
                                    if (valid_symbols[NEWLINE]) {
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    }
                                }
                            } else if (scanner->indents.len > 1 &&
                                       valid_symbols[DEDENT]) {
                                // Non-inline top (or inline top when NEWLINE is not
                                // valid, e.g. inside a type expression), dedent across
                                // a newline: pop the INDENT entry and emit DEDENT.
                                // When the popped entry was inline (e.g. a `(let …)`
                                // block inside a Pi type), set after_cross_newline_
                                // inline_dedent so that the same-line tokens after `)`
                                // do not spuriously push another inline INDENT.
                                bool top_was_inline =
                                    INDENT_IS_INLINE(VEC_BACK(scanner->indents));
                                bool top_converted =
                                    INDENT_IS_CONVERTED(VEC_BACK(scanner->indents));
                                bool top_is_paren =
                                    INDENT_IS_PAREN(VEC_BACK(scanner->indents));
                                // Column of the block popped here (the sibling that
                                // sat immediately above the entries pop_outer_to_column
                                // will pop).  Used to recognise `let x = … λ where
                                // <deeper clauses>` as a do-statement.  Only forwarded
                                // when the sibling is a non-converted layout block.
                                uint16_t top_col =
                                    INDENT_COLUMN(VEC_BACK(scanner->indents));
                                VEC_POP(scanner->indents);
                                if (top_was_inline) {
                                    // Inline INDENT entry (e.g. `let`'s optional block)
                                    // closes without emitting a token.  Emitting DEDENT
                                    // here would extend the `let` node's span to the
                                    // next-line position [L, C], corrupting parse ranges.
                                    // Instead, directly queue DEDENT for each outer block
                                    // that still needs closing (no qdc — avoids extra DEDENTs
                                    // that fire after NEWLINE and prematurely close enclosing
                                    // blocks like `private`), then queue NEWLINE.
                                    //
                                    // The popped inline entry's own DEDENT is emitted
                                    // directly here; outer blocks closed alongside it
                                    // are deferred via qdc.  (Under the close-token
                                    // unification this is uniform for `λ where` and
                                    // `let` inline entries — both close with `_dedent`.)
                                    //
                                    // Mark after_cross_newline_inline_dedent so the
                                    // same-line tokens after a closing `)` do not push a
                                    // spurious inline INDENT, but only when LAMBDA_NEWLINE
                                    // is not valid (a λ-where close, not a let body).
                                    if (!valid_symbols[LAMBDA_NEWLINE]) {
                                        scanner->after_cross_newline_inline_dedent = true;
                                    }
                                    bool any_noninline_popped = false;
                                    bool last_outer_converted = false;
                                    // Defer every outer block popped here via qdc; the
                                    // popped inline entry's own DEDENT is emitted
                                    // directly below.
                                    while (scanner->indents.len > 1 &&
                                           indent_length <
                                               INDENT_COLUMN(VEC_BACK(scanner->indents))) {
                                        last_outer_converted =
                                            INDENT_IS_CONVERTED(VEC_BACK(scanner->indents));
                                        bool _wl_is_inline =
                                            INDENT_IS_INLINE(VEC_BACK(scanner->indents));
                                        uint16_t _wl_col =
                                            INDENT_COLUMN(VEC_BACK(scanner->indents));
                                        if (!_wl_is_inline) {
                                            any_noninline_popped = true;
                                        }
                                        VEC_POP(scanner->indents);
                                        qdc_increment_with_min_col(
                                            scanner, _wl_is_inline, _wl_col,
                                            (uint16_t)indent_length);
                                    }
                                    if (any_noninline_popped) {
                                        // Emit DEDENT for the popped inline entry; the
                                        // outer-block DEDENTs come via qdc and are
                                        // drained by the phantom-NEWLINE mechanism in
                                        // done_enqueue.
                                        QUEUE_PUSH(scanner->tokens, DEDENT);
                                        // Suppress trailing NEWLINE when `where`
                                        // follows: the NEWLINE would commit a
                                        // declaration separator, ending the function
                                        // before the where-clause can attach.
                                        // Without this, `λ where (p) → let n = f
                                        // in\n    g h\n where instTel = x` produces
                                        // a phantom function with MISSING qid that
                                        // owns the where instead of goᶜ.
                                        bool _next_is_where_nx =
                                            (lexer->lookahead == 'w') &&
                                            peek_word_keyword(lexer, "where");
                                        bool suppress =
                                            (!valid_symbols[NEWLINE] && last_outer_converted) ||
                                            _next_is_where_nx;
                                        if (!suppress) {
                                            QUEUE_PUSH(scanner->tokens, NEWLINE);
                                        }
                                    } else if (indent_length <=
                                                    (int)INDENT_COLUMN(VEC_BACK(scanner->indents)) ||
                                                !valid_symbols[LAMBDA_NEWLINE]) {
                                        // Inline lambda block with no outer blocks to close.
                                        // Emit DEDENT+NEWLINE when:
                                        //  • We are at or below the outer block's column
                                        //    (indent_length <= outer_col): the next token
                                        //    is a sibling declaration, not another lambda
                                        //    clause.  LAMBDA_NEWLINE would be incorrect even
                                        //    if the grammar nominally accepts it.
                                        //  • LAMBDA_NEWLINE is not valid at all: the grammar
                                        //    has already exited the lambda context.
                                        //
                                        // Exception: nested inline `let` entries (e.g.
                                        //   `λ { p → let a = x in let b = y in body }`)
                                        // When no non-inline block was popped by the
                                        // while loop above (any_noninline_popped=false)
                                        // and the body still continues within the
                                        // enclosing scope (indent_length > outer layout
                                        // col), do NOT emit DEDENT — that would
                                        // prematurely close the lets.  Return false so
                                        // the expression body is allowed to continue; the
                                        // `}` / `)` handler will pop the inline entries
                                        // when the brace is found.
                                        if (!any_noninline_popped &&
                                            indent_length >
                                                (int)INDENT_COLUMN(VEC_BACK(scanner->indents))) {
                                            goto done_enqueue;
                                        }
                                        QUEUE_PUSH(scanner->tokens, DEDENT);
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    } else if (valid_symbols[LAMBDA_NEWLINE]) {
                                        // indent_length > outer block column: normally
                                        // another lambda clause follows.  Exceptions:
                                        // • `;` / `}` — outer record separator/close;
                                        //   emit DEDENT to close the lambda block.
                                        // • `where` keyword — function-level where clause
                                        //   (e.g. `case x of λ where … \n  where f = …`);
                                        //   emit DEDENT so the where is parsed correctly.
                                        bool _lnl_closes =
                                            (lexer->lookahead == ';' ||
                                             lexer->lookahead == '}') ||
                                            ((lexer->lookahead == 'w') &&
                                             peek_word_keyword(lexer, "where"));
                                        if (_lnl_closes && valid_symbols[DEDENT]) {
                                            QUEUE_PUSH(scanner->tokens, DEDENT);
                                        } else {
                                            QUEUE_PUSH(scanner->tokens, LAMBDA_NEWLINE);
                                        }
                                    } else if (valid_symbols[NEWLINE]) {
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    }
                                } else {
                                    // Non-inline top: pop outer entries via counters
                                    // for deferred multi-level closure, emit DEDENT
                                    // (or DEDENT when only that token is accepted),
                                    // then NEWLINE.  Suppress NEWLINE only for CONVERTED
                                    // tops in brace/paren contexts.
                                    //
                                    // Two sub-cases for popped INLINE entries:
                                    //  • DECL_INLINE (N=1 at push): a declaration-level
                                    //    where-block (e.g. `foo = bar where go = λ where`)
                                    //    → drain as DEDENT via queued_inline_where_count.
                                    //  • Non-DECL_INLINE (N=0 at push): an expression-level
                                    //    inline block (e.g. stranded `let` zombie) → drain
                                    //    as DEDENT via queued_inline_let_count.
                                    // Forward the just-popped sibling column only
                                    // when it was a non-converted layout block; the
                                    // converted (inverted-indent) shape must keep the
                                    // DEDENT routing.
                                    pop_outer_to_column_sib(
                                        scanner, (uint16_t)indent_length,
                                        top_converted ? 0xFFFF : top_col);
                                    QUEUE_PUSH(scanner->tokens, DEDENT);
                                    bool _next_is_where_d =
                                        (lexer->lookahead == 'w') &&
                                        peek_word_keyword(lexer, "where");
                                    // When the popped block is a `let` closing at
                                    // its `in` (cross-newline, `in` below the
                                    // bindings), the `_let_body` follows the
                                    // `_dedent` directly — suppress the separator
                                    // NEWLINE, which would otherwise split the body
                                    // expression's continuation lines.
                                    bool _next_is_in_d =
                                        (lexer->lookahead == 'i') &&
                                        peek_word_keyword(lexer, "in");
                                    bool suppress_newline =
                                        (!valid_symbols[NEWLINE] && top_converted) ||
                                        (!valid_symbols[NEWLINE] && top_is_paren &&
                                         scanner->queued_dedent_count == 0 &&
                                         indent_length >
                                             (int)INDENT_COLUMN(
                                                 VEC_BACK(scanner->indents))) ||
                                        _next_is_where_d ||
                                        _next_is_in_d;
                                    if (!suppress_newline) {
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    }
                                }
                            } else {
                                // D=0, LD=0: grammar not yet ready for DEDENT.
                                // When NEWLINE is valid AND the top is a non-inline,
                                // non-paren layout block, emit NEWLINE first (to
                                // advance the grammar past the current separator
                                // requirement so DEDENT becomes valid), then pre-queue
                                // the DEDENT and further closures.  Without this, the
                                // next scanner call sees skipped_newline=false and
                                // never re-enters the dedent path, so col14's block
                                // stays open and swallows outer declarations.
                                if (valid_symbols[NEWLINE] &&
                                    scanner->indents.len > 1 &&
                                    !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                    !INDENT_IS_PAREN(VEC_BACK(scanner->indents))) {
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    VEC_POP(scanner->indents);
                                    pop_outer_to_column(scanner,
                                                        (uint16_t)indent_length);
                                    QUEUE_PUSH(scanner->tokens, DEDENT);
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                } else if (valid_symbols[NEWLINE]) {
                                    // When the top is a stale inline let entry and
                                    // `where` follows, do not emit NEWLINE — that
                                    // would close the function before its `where`
                                    // clause is parsed.  Pop the stale entry silently
                                    // and return false so the built-in lexer sees
                                    // `where` at the function level.
                                    if (INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                        (lexer->lookahead == 'w') &&
                                        peek_word_keyword(lexer, "where")) {
                                        VEC_POP(scanner->indents);
                                        goto done_enqueue;
                                    }
                                    // When the top is a PAREN (expression-level) block
                                    // — such as a `do` block started inside a function
                                    // body — and `where` follows at a column BELOW the
                                    // block's statements, emit NEWLINE (the grammar's
                                    // last-statement separator) and DEDENT (the block
                                    // closer) together in one call.  Emitting only
                                    // NEWLINE here and deferring DEDENT to the next
                                    // call causes a GLR state divergence: the grammar
                                    // advances a source-file-level stack past the
                                    // function boundary on the NEWLINE, so by the time
                                    // DEDENT arrives, the function's optional(where)
                                    // production is no longer reachable and `where`
                                    // is mis-parsed as a new declaration.  Combining
                                    // NEWLINE+DEDENT in one scan call keeps the
                                    // function's where-clause production active through
                                    // the DEDENT, matching the same-level `where`
                                    // detection behavior.
                                    if (INDENT_IS_PAREN(VEC_BACK(scanner->indents)) &&
                                        !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                        (lexer->lookahead == 'w') &&
                                        peek_word_keyword(lexer, "where")) {
                                        VEC_POP(scanner->indents);
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                        QUEUE_PUSH(scanner->tokens, DEDENT);
                                        goto done_enqueue;
                                    }
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                }
                            }
                        } else {
                            // Same-level crossing of a newline: emit NEWLINE
                            // unless DEDENT is also valid AND the current top is a
                            // real (non-inline) indent entry. That combination means
                            // we are in an optional-DEDENT context (e.g. _let_body)
                            // where the body expressions are at the same column as
                            // the let declarations. Suppressing the NEWLINE allows
                            // them to be parsed as one continued application.
                            //
                            // Exception: `where` at the same column is a layout-
                            // closing keyword that terminates the current block
                            // (e.g. after a `do` block's last stmt). Emit DEDENT so
                            // the grammar can match it as a function where-clause.
                            // No NEWLINE is emitted so `where` is not stranded at
                            // source_file level as a spurious declaration separator.
                            //
                            // When the top IS inline (e.g. inside a paren-wrapped
                            // `(let open X\n  open Y)`), we must emit NEWLINE so the
                            // declarations stay separated inside the paren context.
                            bool top_is_inline = INDENT_IS_INLINE(VEC_BACK(scanner->indents));
                            // `in` at the SAME column as the let-declarations block
                            // (e.g. `let\n  x = a\n  in x` — the `in` aligned with the
                            // bindings) closes the let.  Emit DEDENT so the grammar's
                            // `optional($._dedent)` before `_let_body` consumes it,
                            // rather than the same-level NEWLINE below (which would
                            // mis-treat `in` as another binding separator).
                            if (INDENT_IS_LET(VEC_BACK(scanner->indents)) &&
                                valid_symbols[DEDENT] &&
                                lexer->lookahead == 'i' &&
                                peek_word_keyword(lexer, "in")) {
                                VEC_POP(scanner->indents);
                                QUEUE_PUSH(scanner->tokens, DEDENT);
                                goto done_enqueue;
                            }
                            // When the top is a CONVERTED entry (e.g. an inner
                            // `do`-block pushed via line-1238 in expression
                            // context like `let x = do y do\n  h\n  in z`),
                            // `in` at the same column closes the block.  Emit
                            // NEWLINE (stmt separator) + DEDENT (close block).
                            // The next scanner call will see `in` with outer
                            // inline entries above indent_length, triggering
                            // True Dedent to cascade closures down to the
                            // let_inline_decl which is absorbed by the let
                            // rule's optional($._dedent) after the body.
                            if (!top_is_inline &&
                                INDENT_IS_CONVERTED(VEC_BACK(scanner->indents)) &&
                                lexer->lookahead == 'i') {
                                bool _is_in = false;
                                lexer->mark_end(lexer);
                                if (lexer->lookahead == 'i') {
                                    lexer->advance(lexer, false);
                                    if (lexer->lookahead == 'n') {
                                        lexer->advance(lexer, false);
                                        int32_t _c_in = lexer->lookahead;
                                        _is_in = !isalnum(_c_in) &&
                                                 _c_in != '_' && _c_in != '\'';
                                    }
                                }
                                if (_is_in) {
                                    // Pop inner CONVERTED entry.
                                    VEC_POP(scanner->indents);
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    QUEUE_PUSH(scanner->tokens, DEDENT);
                                    // Pop one more INLINE non-DECL_INLINE entry
                                    // (the immediately-enclosing do_inline) and
                                    // emit its closure.
                                    if (scanner->indents.len > 1) {
                                        uint32_t _e =
                                            VEC_BACK(scanner->indents);
                                        if (INDENT_IS_INLINE(_e) &&
                                            !INDENT_IS_DECL_INLINE(_e)) {
                                            VEC_POP(scanner->indents);
                                            QUEUE_PUSH(scanner->tokens, NEWLINE);
                                            QUEUE_PUSH(scanner->tokens, DEDENT);
                                        }
                                    }
                                    // Pop any remaining outer inline entries
                                    // silently (let_inline absorbed by
                                    // optional($._dedent) after body).
                                    while (scanner->indents.len > 1) {
                                        uint32_t _e =
                                            VEC_BACK(scanner->indents);
                                        if (!INDENT_IS_INLINE(_e)) break;
                                        VEC_POP(scanner->indents);
                                    }
                                    goto done_enqueue;
                                }
                            }
                            if (valid_symbols[DEDENT] && !top_is_inline &&
                                lexer->lookahead == 'w') {
                                bool _next_is_where =
                                    peek_word_keyword(lexer, "where");
                                if (_next_is_where) {
                                    VEC_POP(scanner->indents);
                                    QUEUE_PUSH(scanner->tokens, DEDENT);
                                } else if (valid_symbols[NEWLINE]) {
                                    // Not `where` — just emit NEWLINE to separate declarations.
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                }
                            } else if (valid_symbols[LAMBDA_NEWLINE]) {
                                QUEUE_PUSH(scanner->tokens, LAMBDA_NEWLINE);
                            } else if (valid_symbols[NEWLINE]) {
                                // Same column → virtual semicolon (Agda's
                                // offsideRule EQ case): always a separator.
                                // If the same-level content is a `--` or `{-`
                                // comment, do not emit NEWLINE yet.  The
                                // comment is consumed as an extra; the
                                // scanner is re-invoked afterwards.  The
                                // actual indentation may be deeper (e.g. a
                                // `where` clause following a comment at the
                                // block level) and should not be treated as
                                // a declaration separator.
                                if (lexer->lookahead == '-') {
                                    // Only update mark_end when the idiom check
                                    // has NOT already peeked past '(' — if it
                                    // has, mark_end is correctly at '(' and
                                    // overwriting it here would move the NEWLINE
                                    // boundary past '(', consuming '(' as part
                                    // of the NEWLINE token.
                                    if (!open_paren_peeked) {
                                        lexer->mark_end(lexer);
                                    }
                                    lexer->advance(lexer, false);
                                    if (lexer->lookahead == '-') {
                                        // Scan for NUL (0x00): the built-in DFA
                                        // uses 0x00 as EOF and stops there, leaving
                                        // a spurious ERROR node.  Consume and emit
                                        // COMMENT externally when NUL is present;
                                        // otherwise return false so the built-in
                                        // rule handles the comment unchanged.
                                        if (valid_symbols[COMMENT]) {
                                            lexer->advance(lexer, false);
                                            bool _has_nul = false;
                                            while (!lexer->eof(lexer) &&
                                                   lexer->lookahead != '\n') {
                                                if (lexer->lookahead == 0)
                                                    _has_nul = true;
                                                lexer->advance(lexer, false);
                                            }
                                            if (_has_nul) {
                                                lexer->mark_end(lexer);
                                                lexer->result_symbol = COMMENT;
                                                return true;
                                            }
                                        }
                                        return false;
                                    }
                                } else if (lexer->lookahead == '{') {
                                    advance(lexer);
                                    if (lexer->lookahead == '-') {
                                        advance(lexer);
                                        if (lexer->lookahead != '#' && valid_symbols[BLOCK_COMMENT]) {
                                            scan_block_comment_body(lexer);
                                            lexer->mark_end(lexer);
                                            lexer->result_symbol = BLOCK_COMMENT;
                                            return true;
                                        } else if (lexer->lookahead != '#') {
                                            return false;
                                        }
                                    }
                                }
                                QUEUE_PUSH(scanner->tokens, NEWLINE);
                            }
                        }
                    } else {
                        // Before `)`/`}`, when the top is a do-block (DO_BLOCK_FLAG)
                        // whose last statement has not yet reduced (DEDENT not valid
                        // but NEWLINE is) — e.g. the last do-statement is an inline
                        // brace-lambda `case x of λ { … }` in `g (do … case x of λ {
                        // … })` — emit a reducing NEWLINE (the do-statement
                        // separator).  This forces the application to reduce so the
                        // do-block's `_dedent` becomes valid, and the `)`/`}` handler
                        // below closes the do-block on the next call.  Without this
                        // the do-block stays open and `)`/`}` is lexed prematurely.
                        if ((lexer->lookahead == ')' || lexer->lookahead == '}') &&
                            scanner->indents.len > 1 &&
                            !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                            (INDENT_IS_DO(VEC_BACK(scanner->indents)) ||
                             INDENT_IS_LAM_WHERE(VEC_BACK(scanner->indents))) &&
                            INDENT_IS_PAREN(VEC_BACK(scanner->indents)) &&
                            valid_symbols[NEWLINE] && !valid_symbols[DEDENT] &&
                            (uint16_t)lexer->get_column(lexer) == indent_length) {
                            QUEUE_PUSH(scanner->tokens, NEWLINE);
                            goto done_enqueue;
                        }
                        // A function `where` at the do-block's column closes the
                        // do-block, but its last statement may not have reduced
                        // (DEDENT not valid, NEWLINE is) — e.g. that statement is
                        // `case … of λ where <clauses>`.  Pop the do-block and queue
                        // NEWLINE then DEDENT: the NEWLINE reduces the statement, the
                        // DEDENT closes the do-block, and `where` attaches to the
                        // enclosing function.
                        // Only a `where` at EXACTLY the do-block's own column is the
                        // function's where-clause; a deeper `where` is a do-statement's
                        // own `do_where` (`p ← e where …`) and must NOT close it.
                        if (lexer->lookahead == 'w' &&
                            scanner->indents.len > 1 &&
                            !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                            INDENT_IS_DO(VEC_BACK(scanner->indents)) &&
                            valid_symbols[NEWLINE] && !valid_symbols[DEDENT] &&
                            indent_length ==
                                INDENT_COLUMN(VEC_BACK(scanner->indents)) &&
                            peek_word_keyword(lexer, "where")) {
                            VEC_POP(scanner->indents);
                            QUEUE_PUSH(scanner->tokens, NEWLINE);
                            QUEUE_PUSH(scanner->tokens, DEDENT);
                            goto done_enqueue;
                        }
                        // At `)` on the same line (possibly after spaces): pop
                        // any open indent block and emit NEWLINE+DEDENT so the
                        // grammar can close blocks inside parenthesised
                        // expressions (e.g. `pure x ) y` closing a do-block).
                        //
                        // Two cases for the top stack entry:
                        //  • Inline (e.g. `let` block in `(let …)`): close only
                        //    when INDENT is not also valid — when INDENT IS
                        //    valid the GLR is still inside a sub-expression
                        //    (e.g. `let x = g (λ {b} hb → b) in x`) and firing
                        //    would close the let block mid-expression.
                        //  • Non-inline (e.g. `do` block started after a
                        //    newline): close only when the top entry carries
                        //    PAREN_FLAG, meaning it was pushed when NEWLINE was
                        //    not valid (i.e. inside a parenthesised context).
                        //    Module-level blocks are pushed when NEWLINE IS
                        //    valid and must NOT be closed by a `)` inside a
                        //    sub-expression, even if valid_symbols[DEDENT] is
                        //    spuriously true under GLR.
                        if ((lexer->lookahead == ')' || lexer->lookahead == '}') &&
                            valid_symbols[DEDENT] &&
                            scanner->indents.len > 1 &&
                            (uint16_t)lexer->get_column(lexer) == indent_length) {
                            // Extend the `)` handler to also fire for `}`.
                            // A `let` inside a record literal `record { … let
                            // x = v in body }` pushes an INLINE INDENT; without
                            // this extension the INDENT is not popped before `}`
                            // and lingers, causing the function's `where` clause
                            // on the next line to be eaten by a spurious DEDENT.
                            //
                            // Also fire when only DEDENT is valid (DEDENT is
                            // false) and the top is an inline entry.  This covers
                            // `(let y = f x in y)` where the let rule's
                            // optional($._dedent) is pending at `)` but DEDENT
                            // is not.  Without this extension the inline INDENT
                            // pushed for `let y` is never popped at `)`, leaving a
                            // stale scanner state that later emits a spurious DEDENT
                            // when the NEXT function's `where` clause is reached.
                            bool _top_inline = INDENT_IS_INLINE(
                                VEC_BACK(scanner->indents));
                            // For `}`: by default only close INLINE entries.  A
                            // non-inline PAREN entry (e.g. a `λ where` body started
                            // after a newline) must NOT be popped by the `}` of an
                            // inner `record { … }` literal — doing so permanently
                            // removes the layout block that closes the λ-where arms.
                            // EXCEPTION: a do-block (DO_BLOCK_FLAG) opened as a
                            // brace-lambda clause body (`λ { p → do … }`) IS closed
                            // by the brace-lambda's `}` — the DO flag distinguishes
                            // it from a record-literal field's λ-where block.
                            // Inline non-DECL entries (pushed inside `{ }` where
                            // NEWLINE was not valid, e.g. `let` inside `λ { … }`)
                            // must always be closed when `}` or `)` is found —
                            // even if INDENT is still valid due to GLR ambiguity.
                            // DECL_INLINE entries (pushed where NEWLINE was valid,
                            // e.g. top-level `let`) must NOT be closed when an
                            // inner `}` appears (e.g. `let x = g (λ {b} → b) in y`
                            // — the `}` closes the λ-brace but must not close the
                            // outer let whose body hasn't ended yet).
                            bool _should_close = _top_inline
                                ? (!valid_symbols[INDENT] ||
                                   !INDENT_IS_DECL_INLINE(VEC_BACK(scanner->indents)))
                                : ((lexer->lookahead != '}' ||
                                    INDENT_IS_DO(VEC_BACK(scanner->indents)) ||
                                    INDENT_IS_LAM_WHERE(VEC_BACK(scanner->indents))) &&
                                   INDENT_IS_PAREN(VEC_BACK(scanner->indents)));
                            bool _popped_was_do =
                                INDENT_IS_DO(VEC_BACK(scanner->indents));
                            if (_should_close) {
                                VEC_POP(scanner->indents);
                                if (valid_symbols[NEWLINE]) {
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                }
                                // Close the inner block with the unified DEDENT.
                                QUEUE_PUSH(scanner->tokens, DEDENT);
                                // When the entry just closed was NOT a do-block but a
                                // `λ where` clause block sitting directly on top of a
                                // do-block (the last do-statement of `(do … caseM x
                                // λ where …)`), the do-block must still close before
                                // `)`.  Its `_dedent` cannot fire yet because the
                                // trailing lambda-application has not reduced (vD=0 at
                                // the `)`).  Queue a NEWLINE after the λ-where DEDENT:
                                // it reduces the do-statement (the NEWLINE is a do-stmt
                                // separator) so the do-block's `_dedent` becomes valid,
                                // and the EXISTING non-inline `)` handler then closes
                                // the do-block on the next scan call.  We do NOT pop
                                // the do-block here — popping by hand cannot tell a
                                // do-block opened inside THESE parens from one merely
                                // in application position (`f x do …`), and would
                                // over-close.  The queued NEWLINE is harmless if
                                // unused: the drain discards a NEWLINE the grammar
                                // does not accept.
                                if (lexer->lookahead == ')' && !_popped_was_do &&
                                    scanner->indents.len > 1 &&
                                    !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                    INDENT_IS_DO(VEC_BACK(scanner->indents))) {
                                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                                }
                            }
                        } else if (indent) {
                            // When `in` keyword appears at a higher column than
                            // the inline-block top, the grammar is inside a
                            // nested inline block (e.g. `let instance z = e in
                            // body` where the instance block pushed its own
                            // inline INDENT).  We need to close that inner block
                            // so `in` is visible to the enclosing `let…in`.
                            //
                            // Two cases:
                            //  1. DEDENT is valid: emit DEDENT immediately.
                            //  2. Only NEWLINE is valid (DEDENT not yet valid):
                            //     emit NEWLINE first to advance the grammar to a
                            //     state where DEDENT becomes valid; the scanner
                            //     will be called again and emit DEDENT then.
                            if ((valid_symbols[DEDENT] || valid_symbols[NEWLINE]) &&
                                scanner->indents.len > 1 &&
                                (INDENT_IS_INLINE(VEC_BACK(scanner->indents)) ||
                                 INDENT_IS_LET(VEC_BACK(scanner->indents))) &&
                                lexer->lookahead == 'i') {
                                lexer->mark_end(lexer);
                                lexer->advance(lexer, false);
                                if (lexer->lookahead == 'n') {
                                    lexer->advance(lexer, false);
                                    int32_t c = lexer->lookahead;
                                    if (c == ' ' || c == '\t' ||
                                        c == '\n' || c == '\r' || c == 0) {
                                        // When the second-to-top stack entry is
                                        // also inline we are inside a nested
                                        // inline block (e.g. `let instance z =
                                        // i in` where the instance block pushed
                                        // its own inline INDENT).  In that case:
                                        //  • DEDENT valid → pop the inner entry
                                        //    and emit DEDENT so the inner block
                                        //    closes (the outer let's scanner is
                                        //    called again for the next `in`).
                                        //  • Only NEWLINE valid → emit NEWLINE
                                        //    to advance the inner grammar past
                                        //    repeat1(seq(sig,$._newline)), then
                                        //    DEDENT will fire on the next call.
                                        //
                                        // When the second-to-top is NOT inline
                                        // we are at the outermost declaration
                                        // level of a `let` (e.g. `(let x = v
                                        // in body)` or `f = let x = v in y`).
                                        // In either branch (DEDENT or NEWLINE),
                                        // emitting a token here would cause the
                                        // token to be consumed by the wrong
                                        // grammar position (DEDENT by an outer
                                        // GLR state, NEWLINE by the decl-repeat
                                        // path producing a zero-width ERROR).
                                        // Return false so the grammar skips the
                                        // optional separator and parses `in`
                                        // directly as _let_body.
                                        bool second_is_inline =
                                            scanner->indents.len > 2 &&
                                            INDENT_IS_INLINE(
                                                scanner->indents.data[
                                                    scanner->indents.len - 2]);
                                        if (second_is_inline) {
                                            if (valid_symbols[DEDENT]) {
                                                VEC_POP(scanner->indents);
                                                QUEUE_PUSH(scanner->tokens, DEDENT);
                                            } else {
                                                QUEUE_PUSH(scanner->tokens, NEWLINE);
                                            }
                                            goto done_enqueue;
                                        } else {
                                            // Outermost `let` block: close it by
                                            // EMITTING the unified `_dedent` right
                                            // before `in` (Agda's `vclose` position,
                                            // its `error{%popBlock}` analogue).  The
                                            // grammar's `optional($._dedent)` that sits
                                            // before `_let_body` consumes it, so the
                                            // close cannot drift past the body and
                                            // steal an enclosing block's DEDENT.
                                            VEC_POP(scanner->indents);
                                            QUEUE_PUSH(scanner->tokens, DEDENT);
                                            goto done_enqueue;
                                        }
                                    }
                                }
                                // Not `in` keyword — fall through.
                            }
                            // If the inline content is a `--` line comment, do
                            // not push an inline INDENT.  Return false so the
                            // built-in lexer consumes it as an extra; the scanner
                            // will then be called again at the next content line.
                            if (lexer->lookahead == '-') {
                                advance(lexer);
                                if (lexer->lookahead == '-') {
                                    return false;
                                }
                                return false;
                            }
                            // Never push an inline INDENT when the lookahead
                            // is ')': that character will immediately trigger
                            // the inline-pop (DEDENT) path, creating a
                            // push/pop cycle that stalls error recovery forever.
                            // Also never push when DEDENT is already valid:
                            // that means we're at an end-of-block position
                            // (e.g. after a closing ')' mid-expression), not
                            // at the start of an inline block like `(let …)`.
                            // Pushing here would create a spurious stack entry
                            // that later generates an unexpected DEDENT.
                            // Also never push when the lookahead is `→` (U+2192)
                            // or `→` could not start a let-block declaration or
                            // do-block statement, so an INDENT at this position
                            // would be spurious (e.g. `(let …) → b`).
                            // Suppress inline INDENT when after_cross_newline_inline_dedent
                            // is set AND NEWLINE is not valid.  The flag is set after a
                            // `(let …\n    )` block closes via cross-newline DEDENT; it
                            // prevents spurious inline INDENT pushes on the same-line tokens
                            // that follow `)` (e.g. `→ b`).  The NEWLINE guard ensures the
                            // suppression is limited to expression contexts (where NEWLINE
                            // is not a valid separator): at declaration level NEWLINE IS
                            // valid, so we do not suppress there (e.g. `private variable`
                            // after `let … in g`).
                            // Do not push an inline INDENT when the lookahead is
                            // the `in` keyword.  At `let X in expr`, after X's
                            // optional block, both INDENT and NEWLINE are valid.
                            // Without this guard the scanner would push a spurious
                            // inline INDENT at the column of `in`; the grammar then
                            // immediately rejects it, producing a zero-width ERROR.
                            // We peek ahead (using mark_end so the position resets
                            // on return false) to confirm it is `in` + whitespace.
                            // The column check ensures this guard only fires when
                            // `i` is genuinely the first character at the current
                            // indent column.  When OPEN_IDIOM peeked past `(` the
                            // lookahead is `i` but at column indent_length+1; the
                            // column mismatch prevents the guard from consuming `(`
                            // as part of the INDENT token in that case.
                            // Suppress inline INDENT for declaration-keyword stacking
                            // (e.g. `private variable`, `postulate instance`, `abstract
                            // record`, `module X where postulate`).  The NEWLINE guard
                            // restricts suppression to declaration-level contexts and
                            // prevents firing inside expression contexts like
                            // `let instance z = i in foo` where NEWLINE is not valid.
                            if (valid_symbols[INDENT] && !valid_symbols[DEDENT] &&
                                valid_symbols[NEWLINE] &&
                                scan_suppress_inline_indent(lexer, indent_length)) {
                                return false;
                            }
                            if (valid_symbols[INDENT] &&
                                lexer->lookahead != ')' &&
                                lexer->lookahead != 0x2192 &&
                                !valid_symbols[DEDENT] &&
                                !valid_symbols[DEDENT] &&
                                !valid_symbols[LAMBDA_NEWLINE] &&
                                !(scanner->after_cross_newline_inline_dedent &&
                                  !valid_symbols[NEWLINE]) &&
                                // Do not push an inline INDENT at the same column
                                // as the current stack top.  After the after-newline
                                // path pushes a regular INDENT at col X and uses
                                // mark_end at X, the scanner is re-invoked at col X
                                // with skipped_newline=false.  Without this guard,
                                // the inline section would push a second INDENT_MAKE
                                // (INLINE) at the same column — one for the outer
                                // _lambda_where_block and one spurious inline entry —
                                // breaking subsequent DEDENT matching.
                                indent_length > INDENT_COLUMN(VEC_BACK(scanner->indents)) &&
                                // Do not push a further inline entry when BOTH the top
                                // AND the second-to-top are non-DECL_INLINE inline entries
                                // AND NEWLINE is not valid.  Two stacked non-DECL_INLINE
                                // entries indicate nested `let` blocks inside an explicit-
                                // brace context (`λ { … }`, `record { … }`).  Pushing
                                // additional inline entries for body atoms in this case
                                // creates spurious DEDENTs that remove the outer let
                                // entries before `}` can close them.
                                // Requiring TWO non-DECL_INLINE entries (not just one)
                                // prevents this guard from blocking valid single-level
                                // inline pushes such as `do let y = x` where only the
                                // do-body inline entry (non-DECL_INLINE because NEWLINE
                                // was false at its push time) sits on the stack.
                                !(scanner->indents.len >= 3 &&
                                  INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                  !INDENT_IS_DECL_INLINE(VEC_BACK(scanner->indents)) &&
                                  INDENT_IS_INLINE(
                                      scanner->indents.data[scanner->indents.len - 2]) &&
                                  !INDENT_IS_DECL_INLINE(
                                      scanner->indents.data[scanner->indents.len - 2]) &&
                                  !valid_symbols[NEWLINE])) {
                                // Set DECL_INLINE_FLAG when NEWLINE is valid at push
                                // time, marking this as a declaration-level inline block
                                // (e.g. `where`-block).  This distinguishes it from
                                // expression-level inline blocks (e.g. `let`-block inside
                                // `record { … }`) whose INLINE entries may be left as
                                // zombies when neither DEDENT nor NEWLINE was valid at
                                // `in`-keyword time, and should NOT receive a DEDENT when
                                // the enclosing lambda-where block closes.
                                //
                                // Exception: do NOT set DECL_INLINE when VEC_BACK is a
                                // PAREN or CONVERTED entry (a lambda body or do-block in
                                // expression context).  Even if NEWLINE happens to be
                                // valid in the GLR union (leaked from outer declaration
                                // stacks), we are actually inside an expression-level
                                // block.  DECL_INLINE here would route the entry through
                                // queued_inline_where_count when an enclosing block
                                // closes, emitting a spurious DEDENT that prematurely
                                // closes subsequent layout constructs like `private`.
                                bool _ve_is_expr_ctx =
                                    INDENT_IS_PAREN(VEC_BACK(scanner->indents)) ||
                                    INDENT_IS_CONVERTED(VEC_BACK(scanner->indents)) ||
                                    (INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                     !INDENT_IS_DECL_INLINE(
                                         VEC_BACK(scanner->indents)));
                                uint32_t _ie = INDENT_MAKE(indent_length, 1);
                                if (valid_symbols[NEWLINE] && !_ve_is_expr_ctx) {
                                    _ie |= DECL_INLINE_FLAG;
                                }
                                // Tag `let`-block inline entries (valid_symbols
                                // [LET_INDENT] is true only right after `let`).
                                if (valid_symbols[LET_INDENT]) {
                                    _ie |= LET_INLINE_FLAG;
                                }
                                // Tag an inline do-block (`do stmt …` first stmt on
                                // the same line as `do`) so its close routes to
                                // queued_inline_do_count, not the discarding
                                // queued_inline_let_count.
                                if (valid_symbols[DO_INDENT]) {
                                    _ie |= DO_BLOCK_FLAG;
                                }
                                VEC_PUSH(scanner->indents, _ie);
                                QUEUE_PUSH(scanner->tokens, INDENT);
                            }
                        } else if (dedent) {
                            // Same-line dedent on an INLINE top.  Three cases:
                            //  (A) DECL_INLINE (declaration-level where-block):
                            //      always do the conversion — needed for the
                            //      `where module Go ... where\n body` pattern.
                            //  (B) Non-DECL_INLINE WITH valid_symbols[NEWLINE]:
                            //      do the conversion — needed for
                            //      `record { f = let open S in x ; g = ... }`
                            //      where `;` closes the let's inline.
                            //  (C) Non-DECL_INLINE WITHOUT NEWLINE valid:
                            //      SKIP the conversion.  We are inside an
                            //      expression containing the let's body
                            //      (e.g. the inner `(` of `λ where (a ∷ b)`
                            //      inside a multi-line let-in body).  The
                            //      let must stay alive; the body completes
                            //      and the `)` / EOF handlers will close it.
                            if (INDENT_IS_INLINE(VEC_BACK(scanner->indents))) {
                                // Inline INDENT: pushed without crossing a
                                // newline (e.g. `where module Go x where`).
                                // If content at the same level continues at
                                // a column *above* the outer block top, replace
                                // the inline entry with a normal one so
                                // subsequent lines are treated as a regular
                                // indented block rather than emitting a premature
                                // DEDENT that would close the outer block.
                                bool _popped_was_decl_inline =
                                    INDENT_IS_DECL_INLINE(
                                        VEC_BACK(scanner->indents));
                                VEC_POP(scanner->indents);
                                uint16_t new_top =
                                    INDENT_COLUMN(VEC_BACK(scanner->indents));
                                if (indent_length > new_top &&
                                    (_popped_was_decl_inline ||
                                     valid_symbols[NEWLINE])) {
                                    // Use CONVERTED_FLAG when NEWLINE is not valid at
                                    // push time (i.e. we're inside a brace/paren
                                    // context such as `λ where` inside `record {…}`).
                                    // CONVERTED_FLAG suppresses the trailing NEWLINE
                                    // at DEDENT time, which would otherwise break the
                                    // surrounding brace-delimited expression.
                                    //
                                    // Gated on _popped_was_decl_inline || NEWLINE:
                                    // skip the conversion when the popped entry was
                                    // a non-DECL_INLINE (let body) AND we are inside
                                    // an expression (no NEWLINE valid).  In that case
                                    // an inner `(` of the let body's expression
                                    // (e.g. `λ where (a ∷ b)`) should NOT trigger
                                    // layout conversion — the let stays alive via
                                    // its body's expression and is closed by the
                                    // `)`/EOF handlers.
                                    uint32_t entry = valid_symbols[NEWLINE]
                                        ? INDENT_MAKE(indent_length, 0)
                                        : INDENT_MAKE_CONVERTED(indent_length);
                                    VEC_PUSH(scanner->indents, entry);
                                    // If the grammar expects an INDENT here
                                    // (e.g. block() after `λ where\n`), emit
                                    // it so the block's $._indent is satisfied.
                                    // Otherwise return false and let the regular
                                    // lexer pick up the next token.
                                    if (valid_symbols[INDENT]) {
                                        QUEUE_PUSH(scanner->tokens, INDENT);
                                    }
                                } else if (indent_length > new_top) {
                                    // Non-DECL_INLINE inline popped, NEWLINE not
                                    // valid: do NOT push a conversion entry.  The
                                    // popped let-body inline is gone; the let body's
                                    // expression continues until `)`/EOF.
                                } else {
                                    // Track whether all further entries are also
                                    // inline (the first entry was inline by this
                                    // branch's precondition).
                                    bool _nnl_inline = true;
                                    while (indent_length < new_top) {
                                        bool _nnl_is_inline =
                                            INDENT_IS_INLINE(
                                                VEC_BACK(scanner->indents));
                                        uint16_t _nnl_col = new_top;
                                        if (!_nnl_is_inline) {
                                            _nnl_inline = false;
                                        }
                                        VEC_POP(scanner->indents);
                                        qdc_increment_with_min_col(
                                            scanner, _nnl_is_inline, _nnl_col,
                                            (uint16_t)indent_length);
                                        new_top = INDENT_COLUMN(
                                            VEC_BACK(scanner->indents));
                                    }
                                    // When only inline let entries were cleared
                                    // and the body continues within the outer
                                    // scope (indent_length > outer layout block),
                                    // return false so the expression body can
                                    // complete; the `}` / `)` handler will close
                                    // the inline entries when the brace arrives.
                                    if (_nnl_inline &&
                                        indent_length >
                                            (int)INDENT_COLUMN(
                                                VEC_BACK(scanner->indents))) {
                                        goto done_enqueue;
                                    }
                                    if (valid_symbols[DEDENT]) {
                                        QUEUE_PUSH(scanner->tokens, DEDENT);
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    } else {
                                        scanner->queued_dedent_count++;
                                    }
                                }
                            } else {
                                bool top_converted =
                                    INDENT_IS_CONVERTED(VEC_BACK(scanner->indents));
                                VEC_POP(scanner->indents);
                                // Cascade-pop further outer entries: DECL_INLINE → qiwc,
                                // everything else → qdc. (Routing the other inlines
                                // through qdc rather than qilc lets the drain still emit
                                // a DEDENT where one is needed, e.g. to close
                                // `do { let X = do … ; stmt }`.)
                                while (indent_length <
                                       INDENT_COLUMN(
                                           VEC_BACK(scanner->indents))) {
                                    uint32_t _e =
                                        VEC_BACK(scanner->indents);
                                    VEC_POP(scanner->indents);
                                    if (INDENT_IS_INLINE(_e) &&
                                        INDENT_IS_DECL_INLINE(_e)) {
                                        scanner->queued_inline_where_count++;
                                    } else {
                                        scanner->queued_dedent_count++;
                                    }
                                }
                                if (valid_symbols[DEDENT]) {
                                    QUEUE_PUSH(scanner->tokens, DEDENT);
                                    // Suppress the trailing NEWLINE when:
                                    //  • the popped block was CONVERTED (inline entry
                                    //    inside a brace context, e.g. `λ where` in
                                    //    `record {…}`), or
                                    //  • the next token is a concrete brace-context
                                    //    separator/closer `;` or `}` (e.g. the second
                                    //    `do` branch of `λ { p → do … ; q → do … }`):
                                    //    `;`/`}` already separate/close the clause, so a
                                    //    virtual NEWLINE here is spurious — and, left in
                                    //    the queue, it would block the INDENT push for
                                    //    the next branch's `do` body.
                                    // Otherwise emit NEWLINE so statement separators are
                                    // preserved.
                                    bool _next_closes_brace =
                                        (lexer->lookahead == ';' ||
                                         lexer->lookahead == '}');
                                    if ((!top_converted || valid_symbols[NEWLINE]) &&
                                        !_next_closes_brace) {
                                        QUEUE_PUSH(scanner->tokens, NEWLINE);
                                    }
                                } else {
                                    scanner->queued_dedent_count++;
                                }
                            }
                        } else if (valid_symbols[DEDENT] &&
                                   scanner->indents.len > 1 &&
                                   !INDENT_IS_INLINE(VEC_BACK(scanner->indents)) &&
                                   lexer->lookahead == 'w') {
                            // `where` at the same column closes the current block
                            // (e.g. `where` after the last stmt in a do-block, once
                            // the stmt's NEWLINE has already been consumed as a
                            // zero-length token and the lexer is positioned here
                            // without crossing a newline in the skip loop).
                            // Guard: top must be a non-inline indent (a real block).
                            // Inline tops indicate we're inside a paren-wrapped
                            // expression (e.g. `λ where (p , q) → body`) — `where`
                            // there is a keyword, not a layout-closing signal.
                            // No NEWLINE is emitted so `where` is not stranded at
                            // source_file level as a spurious declaration separator.
                            if (peek_word_keyword(lexer, "where")) {
                                VEC_POP(scanner->indents);
                                QUEUE_PUSH(scanner->tokens, DEDENT);
                            }
                        }
                    }
                }
            }
        }
    }
done_enqueue:

    if (QUEUE_EMPTY(scanner->tokens)) {
        return false;
    }

    {
        uint16_t tok = QUEUE_FRONT(scanner->tokens);
        QUEUE_POP(scanner->tokens);
        // A queued NEWLINE paired with a DEDENT may need adjustment when the
        // outer grammar context does not accept NEWLINE (e.g. inside a
        // _lambda_where_block, which uses LAMBDA_NEWLINE as clause separator):
        //
        //  • Multi-level DEDENT in a non-NEWLINE context — either because
        //    more entries are queued in queued_dedent_count, or because we
        //    are at EOF and the EOF handler only pops one level at a time.
        //    In both cases the NEWLINE between the two DEDENTs is spurious;
        //    skip it and fire the next DEDENT directly so the enclosing block
        //    (e.g. _lambda_where_block) closes cleanly.
        //
        //  • Single-level DEDENT inside λ-where (e.g. a do-block between two
        //    clauses): convert NEWLINE → LAMBDA_NEWLINE so the repeat
        //    separator is satisfied and the next clause can be parsed.
        if (tok == NEWLINE && !valid_symbols[NEWLINE] && valid_symbols[DEDENT]) {
            if (scanner->queued_dedent_count > 0 &&
                (scanner->queued_dedent_min_col == 0xFFFF ||
                 (uint16_t)lexer->get_column(lexer) <
                     scanner->queued_dedent_min_col)) {
                scanner->queued_dedent_count--;
                if (scanner->queued_dedent_count == 0) {
                    scanner->queued_dedent_min_col = 0xFFFF;
                }
                lexer->result_symbol = DEDENT;
                // After closing a lambda/do block via NEWLINE→DEDENT, the
                // outer declaration block (e.g. `private`, module `where`)
                // still needs a NEWLINE before its own DEDENT (to satisfy
                // block()'s trailing choice(_newline, ';')).  Always re-queue
                // NEWLINE: when qdc>0 it converts to another DEDENT (if still
                // inside λ where) or fires as-is once N becomes valid; when
                // qdc==0 the outer block is at the same column as the next
                // declaration (e.g. `completeness` matching module N's block),
                // and the re-queued NEWLINE fires in the next call where N=1.
                QUEUE_PUSH(scanner->tokens, NEWLINE);
                return true;
            }
            // Inline WHERE-block entry: the NEWLINE that follows the first DEDENT
            // (which closed the λ where clause block) must not be converted to
            // LAMBDA_NEWLINE below — that would trap the parser inside the lambda
            // context and prevent the outer _declaration_block from closing.
            // Intercept here: convert NEWLINE → DEDENT to close the inline
            // _declaration_block, then re-queue NEWLINE for the outer separator.
            if (scanner->queued_inline_where_count > 0) {
                scanner->queued_inline_where_count--;
                lexer->result_symbol = DEDENT;
                QUEUE_PUSH(scanner->tokens, NEWLINE);
                return true;
            }
            if (lexer->eof(lexer) && scanner->indents.len > 1) {
                VEC_POP(scanner->indents);
                lexer->result_symbol = DEDENT;
                return true;
            }
        }
        if (tok == NEWLINE && !valid_symbols[NEWLINE] && valid_symbols[LAMBDA_NEWLINE]) {
            tok = LAMBDA_NEWLINE;
        }
        // Discard a queued NEWLINE when the current grammar state accepts neither
        // NEWLINE nor DEDENT nor LAMBDA_NEWLINE.  This can happen when the NEWLINE
        // was queued as a trailing separator after the final DEDENT that closes
        // source_file's block() branch (e.g. top-level declarations indented at
        // column > 0).  Emitting it would cause a spurious error in the state
        // reached after the outer block closes.
        if (tok == NEWLINE &&
            !valid_symbols[NEWLINE] &&
            !valid_symbols[DEDENT] &&
            !valid_symbols[LAMBDA_NEWLINE]) {
            return false;
        }
        lexer->result_symbol = tok;
        return true;
    }
}

unsigned tree_sitter_agda_external_scanner_serialize(void *payload,
                                                     char *buffer) {
    Scanner *scanner = (Scanner *)payload;

    // Serialization format:
    //   byte 0: bits [6:0] = queued_dedent_count, bit 7 = after_cross_newline_inline_dedent
    //   byte 1: queued_inline_let_count
    //   byte 2: queued_inline_where_count
    //   byte 3: queued_inline_do_count
    //   bytes 4-5: queued_dedent_min_col (uint16_t, little-endian)
    //   byte 6: token queue length (number of queued uint16_t tokens, 0..255)
    //   bytes 7..7+2*qlen-1: token queue entries (each uint16_t, 2 bytes, little-endian)
    //   bytes 7+2*qlen..end: indent stack entries (each uint32_t, 4 bytes)
    // The token_queue must be serialized so that each GLR stack carries its own
    // pending-token state across serialize/deserialize boundaries.
    uint32_t qlen = scanner->tokens.tail - scanner->tokens.head;
    if (scanner->indents.len * sizeof(uint32_t) + 7 + qlen * sizeof(uint16_t) >
        TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        return 0;
    }

    unsigned size = 0;

    buffer[size++] = (char)(
        (scanner->queued_dedent_count & 0x7F) |
        (scanner->after_cross_newline_inline_dedent ? (char)0x80 : 0));

    buffer[size++] = (char)(scanner->queued_inline_let_count & 0xFF);

    buffer[size++] = (char)(scanner->queued_inline_where_count & 0xFF);

    buffer[size++] = (char)(scanner->queued_inline_do_count & 0xFF);

    memcpy(&buffer[size], &scanner->queued_dedent_min_col, sizeof(uint16_t));
    size += sizeof(uint16_t);

    buffer[size++] = (char)(qlen & 0xFF);
    for (uint32_t i = 0; i < qlen; i++) {
        uint16_t tok = scanner->tokens.data[
            (scanner->tokens.head + i) % scanner->tokens.cap];
        memcpy(&buffer[size], &tok, sizeof(uint16_t));
        size += sizeof(uint16_t);
    }

    memcpy(&buffer[size], scanner->indents.data,
           scanner->indents.len * sizeof(uint32_t));
    size += (unsigned)(scanner->indents.len * sizeof(uint32_t));

    return size;
}

void tree_sitter_agda_external_scanner_deserialize(void *payload,
                                                   const char *buffer,
                                                   unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->queued_dedent_count = 0;
    scanner->queued_inline_let_count = 0;
    scanner->queued_inline_where_count = 0;
    scanner->queued_inline_do_count = 0;
    scanner->after_cross_newline_inline_dedent = false;
    scanner->queued_dedent_min_col = 0xFFFF;
    VEC_CLEAR(scanner->indents);
    QUEUE_CLEAR(scanner->tokens);

    if (length == 0) {
        if (buffer == NULL) {
            VEC_PUSH(scanner->indents, INDENT_MAKE(0, 0));
        }
        return;
    }

    // Unpack byte 0: low 7 bits = queued_dedent_count, high bit = flag.
    scanner->queued_dedent_count = (uint8_t)(buffer[0] & 0x7F);
    scanner->after_cross_newline_inline_dedent = ((uint8_t)buffer[0] & 0x80) != 0;

    unsigned size = 1;

    // Byte 1: queued_inline_let_count (present in format v2+).
    if (length > size) {
        scanner->queued_inline_let_count = (uint8_t)buffer[size++];
    }

    // Byte 2: queued_inline_where_count (present in format v3+).
    if (length > size) {
        scanner->queued_inline_where_count = (uint8_t)buffer[size++];
    }

    // Byte 3: queued_inline_do_count.
    if (length > size) {
        scanner->queued_inline_do_count = (uint8_t)buffer[size++];
    }

    // Bytes 4-5: queued_dedent_min_col (uint16_t LE).
    if (length >= size + (unsigned)sizeof(uint16_t)) {
        memcpy(&scanner->queued_dedent_min_col, &buffer[size],
               sizeof(uint16_t));
        size += sizeof(uint16_t);
    }

    // Byte 6: token queue length.
    // Each entry is a uint16_t (2 bytes, little-endian).
    if (length > size) {
        uint8_t qlen = (uint8_t)buffer[size++];
        for (uint8_t i = 0; i < qlen; i++) {
            if (length >= size + (unsigned)sizeof(uint16_t)) {
                uint16_t tok;
                memcpy(&tok, &buffer[size], sizeof(uint16_t));
                size += sizeof(uint16_t);
                QUEUE_PUSH(scanner->tokens, tok);
            }
        }
    }

    if (length > size) {
        VEC_GROW(scanner->indents,
                 (uint32_t)(length - size) / sizeof(uint32_t));
        scanner->indents.len = (length - size) / sizeof(uint32_t);
        memcpy(scanner->indents.data, &buffer[size],
               scanner->indents.len * sizeof(uint32_t));
        size += (unsigned)(scanner->indents.len * sizeof(uint32_t));
    }

    if (scanner->indents.len == 0) {
        VEC_PUSH(scanner->indents, INDENT_MAKE(0, 0));
        return;
    }

    assert(size == length);
}

void *tree_sitter_agda_external_scanner_create() {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    scanner->indents = indent_vec_new();
    scanner->tokens = token_queue_new();
    tree_sitter_agda_external_scanner_deserialize(scanner, NULL, 0);
    return scanner;
}

void tree_sitter_agda_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    VEC_FREE(scanner->indents);
    QUEUE_FREE(scanner->tokens);
    free(scanner);
}
