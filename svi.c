/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <http://unlicense.org/>
 */

/*
 * TODO (from highest to lowest priority):
 * - switch away from termbox2
 * - command mode
 * - writing to file
 * - dont let cursor go past text
 * - UTF-8 support
 * - maybe try to handle OOM more gracefully instead of exiting instantly
 */

/*
 * ===========================================================================
 * includes
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TB_IMPL
#include "termbox2.h"

/*
 * ===========================================================================
 * macros and types
 */

/* constant values */
#define INITIAL_BUFFER_ROWS 32 /* do not make this 0, stuff will break */
#define BUF_SIZE_INCREMENT 16  /* do not make this 0, stuff will break */

#define INITIAL_ROW_SIZE 128   /* do not make this 0 or 1, stuff will break */
#define ROW_SIZE_INCREMENT 64  /* do not make this 0, stuff will break */

/* utility */
#define roundupto(x, multiple) (((x + multiple - 1) / multiple) * multiple)

struct str {
	char *s;
	size_t len, size;
};

struct buf {
	struct str **b;
	size_t size;
};

/*
 * ===========================================================================
 * function declarations
 */

/* memory allocation */
static void *ecalloc(size_t nmemb, size_t size);
static void *emalloc(size_t size);
static void *erealloc(void *ptr, size_t size);
static void *ereallocarray(void *ptr, size_t nmemb, size_t size);

/* errors */
static void die(const char *fmt, ...);

/* strings */
static void str_insertchar(struct str *str, char c, size_t index);
static void str_removechar(struct str *str, size_t index);

/* buffer management */
static void buf_create(struct buf *buf, size_t size);
static void buf_free(const struct buf *buf);
static void buf_resize(struct buf *buf, size_t size);
static void buf_elem_insertchar(struct buf *buf, size_t elem, char c,
		size_t index);
static void buf_elem_removechar(struct buf *buf, size_t elem, size_t index);

/* drawing */
static void draw_row(const struct buf *buf, const char *emptystr, int y);

/* main program loop */
static void run(void);

/*
 * ===========================================================================
 * global variables
 */
static const char *argv0 = NULL;
static int tb_initialized = 0;

/*
 * ===========================================================================
 * memory allocation
 */
static void *
ecalloc(size_t nmemb, size_t size)
{
	void *ret = calloc(nmemb, size);
	if (!ret)
		die("calloc: out of memory");
	return ret;
}

static void *
emalloc(size_t size)
{
	void *ret = malloc(size);
	if (!ret)
		die("malloc: out of memory");
	return ret;
}

static void *
erealloc(void *ptr, size_t size)
{
	void *ret = realloc(ptr, size);
	if (!ret)
		die("realloc: out of memory");
	return ret;
}

static void *
ereallocarray(void *ptr, size_t nmemb, size_t size)
{
	/* overflow checking taken from musl's calloc implementation */
	if (size > 0 && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		die("reallocarray: out of memory");
	}
	return erealloc(ptr, nmemb * size);
}

/*
 * ===========================================================================
 * errors
 */
static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (tb_initialized)
		tb_shutdown();
	if (fmt) {
		if (argv0)
			fprintf(stderr, "%s: ", argv0);
		vfprintf(stderr, fmt, ap);
		if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
			fprintf(stderr, " %s\n", strerror(errno));
		else
			putc('\n', stderr);
	}
	va_end(ap);
	exit(1);
}

/*
 * ===========================================================================
 * strings
 */
static void
str_insertchar(struct str *str, char c, size_t index)
{
	if (str->len + 1 >= str->size) {
		/* if the string is too small, increase its size */
		str->size += ROW_SIZE_INCREMENT;
		str->s = erealloc(str->s, str->size);
	}
	if (index > str->len)
		index = str->len;

	if (str->len == index) {
		/* if we need to insert at the end, simply do that */
		str->s[index] = c;
		str->s[index + 1] = '\0';
		++str->len;
	} else {
		/*
		 * if we need to insert elsewhere, move the portion
		 * of the string beginning from the index forwards
		 * to make room for the character
		 */
		memmove(str->s + index + 1, str->s + index,
				str->len - index + 1);
		str->s[index] = c;
		++str->len;
	}
}

static void
str_removechar(struct str *str, size_t index)
{
	if (str->len == 0)
		return;

	if (index > str->len)
		index = str->len;

	if (str->len - 1 == index) {
		/* if we need to remove at the end, just do that */
		str->s[index] = '\0';
		--str->len;
	} else {
		/*
		 * if we need to remove elsewhere, shift the portion
		 * of the string beginning from (index + 1) backwards
		 */
		memmove(str->s + index, str->s + index + 1, str->len - index);
		--str->len;
	}
}
/*
 * ===========================================================================
 * buffer management
 */
static void
buf_create(struct buf *buf, size_t size)
{
	buf->b = ecalloc(size, sizeof(struct str *));
	buf->size = size;
}

static void
buf_free(const struct buf *buf)
{
	size_t i = 0;
	for (; i < buf->size; ++i) {
		if (buf->b[i]) {
			free(buf->b[i]->s);
			free(buf->b[i]);
		}
	}
	free(buf->b);
}

static void
buf_resize(struct buf *buf, size_t size)
{
	if (size != buf->size) {
		size_t oldsize = buf->size;
		if (size < oldsize) {
			size_t i = oldsize - 1;
			for (; i >= size; --i) {
				if (buf->b[i]) {
					free(buf->b[i]->s);
					free(buf->b[i]);
				}
			}
		}

		buf->b = ereallocarray(buf->b, size, sizeof(struct str *));
		buf->size = size;

		if (size > oldsize) {
			size_t i = oldsize;
			for (; i < size; ++i) {
				buf->b[i] = NULL;
			}
		}
	}
}

static void
buf_elem_insertchar(struct buf *buf, size_t elem, char c, size_t index)
{
	if (!buf->b[elem]) {
		buf->b[elem] = emalloc(sizeof(struct str));
		buf->b[elem]->s = emalloc(INITIAL_ROW_SIZE);
		buf->b[elem]->s[0] = c;
		buf->b[elem]->s[1] = '\0';
		buf->b[elem]->len = 1;
		buf->b[elem]->size = INITIAL_ROW_SIZE;
	} else {
		str_insertchar(buf->b[elem], c, index);
	}
}

static void
buf_elem_removechar(struct buf *buf, size_t elem, size_t index)
{
	if (!buf->b[elem]) {
		buf->b[elem] = emalloc(sizeof(struct str));
		buf->b[elem]->s = emalloc(INITIAL_ROW_SIZE);
		buf->b[elem]->s[0] = '\0';
		buf->b[elem]->len = 0;
		buf->b[elem]->size = INITIAL_ROW_SIZE;
	} else {
		str_removechar(buf->b[elem], index);
	}
}

/*
 * ===========================================================================
 * drawing
 */
static void
draw_row(const struct buf *buf, const char *emptystr, int y)
{
	tb_print(0, y, TB_DEFAULT, TB_DEFAULT, emptystr);
	tb_print(0, y, TB_DEFAULT, TB_DEFAULT, buf->b[y]->s);
}

/*
 * ===========================================================================
 * main program loop
 */
static void
run(void)
{
	/* the main buffer; this is where the text is stored. */
	struct buf buf;

	/* window dimensions and the cursor's current position. */
	int w = tb_width(), h = tb_height();
	int x = 0, y = 0;

	/* variables used to store return values. */
	struct tb_event ev;
	int ret;

	/* if this is true, the loop will finish. */
	int done = 0;

	size_t i = 0; /* DEBUG */

	/* used by draw_row() to clear a single elem. */
	char *emptystr = emalloc((size_t)w);
	memset(emptystr, ' ', (size_t)w);
	emptystr[w - 1] = '\0';

	buf_create(&buf, INITIAL_BUFFER_ROWS);

	while (!done) {
		ret = tb_poll_event(&ev);
		if (ret != TB_OK) {
			if (ret == TB_ERR_POLL && tb_last_errno() == EINTR) {
				/*
				 * poll was interrupted, possibly by a
				 * SIGWINCH; try again
				 */
				continue;
			}
			/* an error occured; exit */
			die("tb_poll_event: %s", tb_strerror(ret));
		}

		switch (ev.type) {
		case TB_EVENT_RESIZE:
			w = tb_width();
			h = tb_height();

			/*
			 * check if current cursor position is now
			 * outside the window, might happen if it
			 * was resized to be smaller
			 */
			if (x > w)
				x = w;
			if (y > h)
				y = h;

			emptystr = erealloc(emptystr, (size_t)w);
			memset(emptystr, ' ', (size_t)w);
			emptystr[w - 1] = '\0';
			break;
		case TB_EVENT_KEY:
			switch (ev.key) {
			case TB_KEY_ESC:
				/* quit */
				done = 1;
				break;
			case TB_KEY_ARROW_UP:
				/* move cursor up */
				if (y > 0) {
					tb_set_cursor(x, --y);
					tb_present();
				}
				break;
			case TB_KEY_ARROW_DOWN:
				/* move cursor down */
				if (y < h) {
					tb_set_cursor(x, ++y);
					tb_present();
				}
				break;
			case TB_KEY_ARROW_RIGHT:
				/* move cursor right */
				if (x < w) {
					tb_set_cursor(++x, y);
					tb_present();
				}
				break;
			case TB_KEY_ARROW_LEFT:
				/* move cursor left */
				if (x > 0) {
					tb_set_cursor(--x, y);
					tb_present();
				}
				break;
			case TB_KEY_ENTER:
				/* go to next line */
				if (y < h) {
					x = 0;
					tb_set_cursor(x, ++y);
					tb_present();
				}
				break;
			case TB_KEY_BACKSPACE2:
				/* remove char behind cursor */
				if (x > 0) {
					buf_elem_removechar(&buf, (size_t)(y),
							(size_t)(x - 1));
					draw_row(&buf, emptystr, y);
					tb_set_cursor(--x, y);
					tb_present();
				}
				break;
			case TB_KEY_DELETE:
				/* remove char at cursor */
				buf_elem_removechar(&buf, (size_t)y,
						(size_t)x);
				draw_row(&buf, emptystr, y);
				tb_present();
				break;
			case 0:
				/* regular key */
				if (ev.ch <= 0x7f) {
					if ((size_t)y >= buf.size)
						buf_resize(&buf,
							(size_t)roundupto(y,
							BUF_SIZE_INCREMENT));
					buf_elem_insertchar(&buf, (size_t)y,
						(char)ev.ch, (size_t)x);
					draw_row(&buf, emptystr, y);
					if (x <= w) {
						++x;
					} else {
						x = 0;
						++y;
					}
					tb_set_cursor(x, y);
					tb_present();
				}
				break;
			}
		}
	}
	free(emptystr);

	/* DEBUG */
	tb_shutdown();
	for (i = 0; i < buf.size; ++i) {
		if (buf.b[i]) {
			puts(buf.b[i]->s);
		} else {
			puts("~");
		}
	}
	buf_free(&buf);
	exit(0);
}

/*
 * ===========================================================================
 * main()
 */
int
main(int argc, char *argv[])
{
	int ret;

	if (argc)
		argv0 = argv[0];

	ret = tb_init();
	if (ret != TB_OK)
		die("failed to init termbox2: %s", tb_strerror(ret));
	tb_initialized = 1;

	run();
	tb_shutdown();
	return 0;
}
