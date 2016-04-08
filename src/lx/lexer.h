/* Generated by lx */

#ifndef LX_H
#define LX_H

enum lx_token {
	TOK_IDENT,
	TOK_TOKEN,
	TOK_AND,
	TOK_PIPE,
	TOK_DOT,
	TOK_DASH,
	TOK_HAT,
	TOK_BANG,
	TOK_TILDE,
	TOK_QMARK,
	TOK_CROSS,
	TOK_STAR,
	TOK_RPAREN,
	TOK_LPAREN,
	TOK_CLOSE,
	TOK_OPEN,
	TOK_MAP,
	TOK_TO,
	TOK_SEMI,
	TOK_BIND,
	TOK_RE,
	TOK_HEX,
	TOK_OCT,
	TOK_ESC,
	TOK_STR,
	TOK_CHAR,
	TOK_EOF,
	TOK_ERROR,
	TOK_UNKNOWN
};

struct lx_pos {
	unsigned byte;
	unsigned line;
	unsigned col;
};

struct lx {
	int (*lgetc)(struct lx *lx);
	void *opaque;

	int c; /* lx_ungetc buffer */

	struct lx_pos start;
	struct lx_pos end;

	void *buf;
	int  (*push) (struct lx *lx, char c);
	void (*pop)  (struct lx *lx);
	int  (*clear)(struct lx *lx);
	void (*free) (struct lx *lx);

	enum lx_token (*z)(struct lx *lx);
};

/*
 * The initial buffer size; this ought to be over the typical token length,
 * so as to avoid a run-up of lots of resizing.
 */
#ifndef LX_DYN_LOW
#define LX_DYN_LOW 1 << 10
#endif

/*
 * High watermark; if the buffer grows over this, it will resize back down
 * by LX_DYN_FACTOR when no longer in use.
 */
#ifndef LX_DYN_HIGH
#define LX_DYN_HIGH 1 << 13
#endif

/*
 * Andrew Koenig said the growth factor should be less than phi, (1 + sqrt(5)) / 2
 * P.J. Plauger said 1.5 works well in practice. (Perhaps because of internal
 * bookkeeping data stored by the allocator.)
 *
 * Non-integer factors here add the constraint that LX_DYN_LOW > 1 because
 * because conversion to size_t truncates, and e.g. 1 * 1.5 == 1 is no good
 * as the requirement is to *increase* a buffer.
 */
#ifndef LX_DYN_FACTOR
#define LX_DYN_FACTOR 2
#endif

/* dynamic token buffer */
struct lx_dynbuf {
	char *p;
	size_t len;
	char *a;
};

const char *lx_name(enum lx_token t);
const char *lx_example(enum lx_token (*z)(struct lx *), enum lx_token t);

void lx_init(struct lx *lx);
enum lx_token lx_next(struct lx *lx);

int lx_fgetc(struct lx *lx);

int  lx_dynpush(struct lx *lx, char c);
void lx_dynpop(struct lx *lx);
int  lx_dynclear(struct lx *lx);
void lx_dynfree(struct lx *lx);

#endif

