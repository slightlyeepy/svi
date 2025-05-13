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
 * todo (from highest to lowest priority):
 * - clean up codebase, shorten some long lines
 * - add support for <count><movement> (e.g 5j to move down 5 rows)
 * - horizontal scrolling for long lines
 * - add support for more movement keys
 * - fix inconsistent coding style; decide on whether 'if (x)' or 'if (x > 0)'
 *   should be used, aswell as 'if (x == 0)' and 'if (!x)'
 * - optimize memory usage on large files
 * - scrolling the screen up redraws the whole screen; is there a more
 *   efficient way to do this?
 * - optimize certain movements to use less escape sequences
 * - UTF-8 support
 * - maybe try to handle OOM more gracefully instead of exiting instantly
 */

/*
 * ============================================================================
 * configurable macros
 */

/*
 * ===================
 * general
 */

/* tab width in columns */
#define TAB_WIDTH       8
#define TAB_WIDTH_CHARS "        "

/* mode for newly created files; will be modified by the process's umask(2) */
#define NEW_FILE_MODE   0666

/*
 * enable usage of non-POSIX/XSI but very common features (sys/ioctl.h,
 * sys/param.h, ioctl, TIOCGWINSZ, and SIGWINCH).
 * 0 = false, 1 = true
 */
#define ENABLE_NONPOSIX 1

/* enable usage of OpenBSD's pledge(2). 0 = false, 1 = true */
#define ENABLE_PLEDGE   0

/*
 * ===================
 * terminal
 */

/* if getting the terminal's size fails, the following size is used instead */
#define FALLBACK_WIDTH  80
#define FALLBACK_HEIGHT 24

/*
 * how long to wait for the terminal's response when getting the size with
 * the fallback method. can't be higher than 999.
 */
#define RESIZE_FALLBACK_MS 500

/*
 * ===================
 * editing buffer
 */

/* how many rows to initially allocate for an empty buffer, can't be 0 */
#define INITIAL_BUFFER_ROWS 32

/* how many rows to add to a buffer's size when it's too small, can't be 0 */
#define BUF_SIZE_INCREMENT  16

/* same as INITIAL_BUFFER_ROWS, but for buffers created from files */
#define FILE_BUFFER_ROWS    128

/*
 * amount of rows to add from a buffer being created from a file that's too
 * small to fit more rows from the file, can't be 0
 */
#define FILE_BUF_SIZE_INCR  256

/*
 * how many columns to initially allocate for each row in an empty buffer,
 * can't be 0 or 1
 */
#define INITIAL_ROW_SIZE    128

/*
 * how many columns to add to a row's size when it's too small,
 * can't be 0 or 1
 */
#define ROW_SIZE_INCREMENT  64

/*
 * how many iovec structures to use when writing to a file.
 * can't be higher than IOV_MAX (guaranteed to be at least 16, but typically
 * a lot higher)
 */
#if ENABLE_NONPOSIX
#define IOV_SIZE            64
#else
#define IOV_SIZE            16
#endif /* ENABLE_NONPOSIX */

/*
 * ===================
 * commands
 */

/*
 * how many characters to initially allocate for the command
 * buffer, can't be 0
 */
#define INITIAL_CMD_SIZE    16

/*
 * how many characters to add to the command buffer's size when it's too
 * small, can't be 0
 */
#define CMD_SIZE_INCREMENT  16

/*
 * ============================================================================
 * includes
 */
#define _XOPEN_SOURCE 700

/* compatibility with certain platforms */
#if ENABLE_NONPOSIX
#include <sys/param.h>

#if defined(BSD)
/* BSD needs _BSD_SOURCE for SIGWINCH and (on OpenBSD) pledge() */
#define _BSD_SOURCE
#endif /* defined(BSD) */

#if defined(__dietlibc__) && defined(__x86_64__) && \
	(defined(__STRICT_ANSI__) || __STDC_VERSION__ < 199900L)
/* needed to work around a bug in dietlibc */
#include <stdint.h>
typedef uint64_t __u64;
#endif /*
	* defined(__dietlibc__) && defined(__x86_64__) && \
	* (defined(__STRICT_ANSI__) || __STDC_VERSION__ < 199900L)
	*/

#endif /* ENABLE_NONPOSIX */

#if ENABLE_NONPOSIX
#include <sys/ioctl.h>
#endif /* ENABLE_NONPOSIX */
#include <sys/select.h>
#include <sys/uio.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#if ENABLE_NONPOSIX
#include <signal.h>
#endif /* ENABLE_NONPOSIX */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/*
 * ============================================================================
 * macros and types
 */

/* terminal */
#define COLOR_DEFAULT NULL
#define COLOR_RESET   "\033[0m"
#define COLOR_BLACK   "\033[30m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

/* buffer management */
#define BUF_ELEM_NOTEMPTY(buf, elem) ((size_t)elem < buf.size && \
		(size_t)elem < buf.len && buf.b[elem] && buf.b[elem]->len)

/* utility */
#define ROUNDUPTO(x, multiple) (((x + multiple - 1) / multiple) * multiple)

/* enums */
enum event_type {
#if ENABLE_NONPOSIX && defined(SIGWINCH)
	TERM_EVENT_RESIZE,
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */
	TERM_EVENT_KEY
};

enum term_key {
	/* esc */
	TERM_KEY_ESC,

	/* movement keys */
	TERM_KEY_ARROW_UP,
	TERM_KEY_ARROW_DOWN,
	TERM_KEY_ARROW_RIGHT,
	TERM_KEY_ARROW_LEFT,
	TERM_KEY_HOME,
	TERM_KEY_END,

	/* misc */
	TERM_KEY_INSERT,
	TERM_KEY_DELETE,
	TERM_KEY_PAGE_UP,
	TERM_KEY_PAGE_DOWN,

	/* backspace/enter */
	TERM_KEY_BACKSPACE,
	TERM_KEY_ENTER,

	/* tab */
	TERM_KEY_TAB,

	/* ctrl+<key> / regular key */
	TERM_KEY_CTRL,
	TERM_KEY_CHAR
};

enum mode {
	MODE_NORMAL,
	MODE_INSERT,
	MODE_COMMAND_LINE
};

/* structs */
struct term_event {
	enum event_type type;
	enum term_key key;
	char ch;
};

struct row {
	char *s;
	size_t len, size;
	size_t tabs;
};

struct buf {
	struct row **b;
	size_t len, size;
};

struct state {
	struct buf buf; /* the main buffer */
	struct row cmd; /* string used to hold commands */

	int w, h; /* window dimensions */
	int x, y; /* cursor's current position in the editing buffer */
	int tx, ty; /* cursor's current position on-screen */

	enum mode mode; /* current mode */
	int storedtx; /* value of tx before entering command-line mode */

	char *name; /* name of file being edited */
	int name_needs_free; /* whether name should be free()'d */
	int modified; /* whether the buffer has unwritten changes */
	int written; /* whether we've written into a file once */

	struct term_event ev; /* current terminal event */
	int done; /* if this is true, the main loop will finish */
};

/*
 * ============================================================================
 * function declarations
 */

/* memory allocation */
static void *ecalloc(size_t nmemb, size_t size);
static void *emalloc(size_t size);
static void *erealloc(void *ptr, size_t size);
static void *ereallocarray(void *ptr, size_t nmemb, size_t size);
static char *estrdup(const char *s);

/* errors */
static void die(const char *fmt, ...);

/* terminal */
static void readkey(struct term_event *ev);
static void term_clear_row(int y);
static void term_event_wait(struct term_event *ev);
static void term_init(void);
static void term_print(int x, int y, const char *color, const char *str);
static void term_printf(int x, int y, const char *color, const char *fmt, ...);
static void term_set_cursor(int x, int y);
static void term_shutdown(void);
static int term_size(int *w, int *h);
static int try_read_chr(char *c);
#if ENABLE_NONPOSIX && defined(SIGWINCH)
static void winch(int unused);
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */

/* strings */
static size_t count_tabs(const char *s, size_t l);

/* rows */
static void row_insertchar(struct row *row, char c, size_t index,
		size_t size_increment);
static void row_removechar(struct row *row, size_t index);

/* buffer management */
static void buf_char_insert(struct buf *buf, size_t elem, char c,
		size_t index);
static void buf_char_remove(struct buf *buf, size_t elem, size_t index);
static void buf_create(struct buf *buf, size_t size);
static size_t buf_elem_len(struct buf *buf, size_t elem);
static void buf_free(const struct buf *buf);
static void buf_resize(struct buf *buf, size_t size);
static void buf_shift_down(struct buf *buf, size_t start_index,
		size_t size_increment);
static void buf_shift_up(struct buf *buf, size_t start_index);

/* buffer file operations */
static int buf_from_file(struct buf *buf, const char *filename);
static int buf_write(const struct buf *buf, const char *filename,
		int overwrite);
static int iov_write(struct iovec *iov, int *iovcnt, size_t iov_size,
		int writefd, char *str, size_t len);

/* movement */
static void cursor_fix_xpos(struct state *st);
static void cursor_up(struct state *st);
static void cursor_down(struct state *st);
static void cursor_right(struct state *st, int stopatlastchar);
static void cursor_left(struct state *st);
static void cursor_linestart(struct state *st);
static void cursor_lineend(struct state *st, int stopbeforelastchar);
static void cursor_startnextrow(struct state *st, int stripextranewline);
static void cursor_endpreviousrow(struct state *st);
static void cursor_nonblank(struct state *st);
static void cursor_up_page(struct state *st);
static void cursor_down_page(struct state *st);

/* commands */
static const char *cmdarg(const char *cmd);
static int cmdchrcmp(const char *cmd, char c);
static int cmdstrcmp(const char *cmd, const char *s, size_t sl);
static int exec_cmd(struct state *st);

/* helper functions */
static void insert_newline(struct state *st);
static void redraw(struct state *st, int start_y, int start_ty, int end_ty);
static void redraw_row(struct state *st, int y, int ty);
static void remove_newline(struct state *st);

/* event handling */
static void key_command_line(struct state *st);
static void key_insert(struct state *st);
static void key_normal(struct state *st);
static void resized(struct state *st);

/* main program loop */
static void run(int argc, char *argv[]);

/*
 * ============================================================================
 * global variables
 */
static const char *argv0 = NULL;

/* 0 = no, 1 = terminal mode set, 2 = stdin set to nonblocking mode */
static int term_init_stage = 0;

/* terminal state */
static struct termios tio, oldtio;
static int old_stdin_flags;

/* SIGWINCH handling */
#if ENABLE_NONPOSIX && defined(SIGWINCH)
static volatile sig_atomic_t win_resized = 0;
static sigset_t oldmask;
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */

/*
 * ============================================================================
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
	if (size && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		die("reallocarray: out of memory");
	}
	return erealloc(ptr, nmemb * size);
}

static char *
estrdup(const char *s)
{
	char *ret = strdup(s);
	if (!ret)
		die("strdup: out of memory");
	return ret;
}

/*
 * ============================================================================
 * errors
 */
static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (term_init_stage)
		term_shutdown();
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
 * ============================================================================
 * terminal
 */
static void
readkey(struct term_event *ev)
{
	/* read a key from stdin and write the data into a term_event. */
	char c;
	for (;;) {
		read(STDIN_FILENO, &c, 1);
		switch (c) {
		case '\033':
			/*
			 * <esc>, might be the start of a multi-character
			 * key sequence
			 */
			if (try_read_chr(&c) && c == '[' && try_read_chr(&c)) {
				switch (c) {
				case 'A':
					/* <esc>[A = up arrow */
					ev->key = TERM_KEY_ARROW_UP;
					return;
				case 'B':
					/* <esc>[B = down arrow */
					ev->key = TERM_KEY_ARROW_DOWN;
					return;
				case 'C':
					/* <esc>[C = right arrow */
					ev->key = TERM_KEY_ARROW_RIGHT;
					return;
				case 'D':
					/* <esc>[D = left arrow */
					ev->key = TERM_KEY_ARROW_LEFT;
					return;
				case 'H':
					/* <esc>[H = home */
					ev->key = TERM_KEY_HOME;
					return;
				case 'F':
					/* <esc>[F = end */
					ev->key = TERM_KEY_END;
					return;
				case '2':
					if (try_read_chr(&c) && c == '~') {
						/* <esc>[2~ = insert */
						ev->key = TERM_KEY_INSERT;
						return;
					}
					break;
				case '3':
					if (try_read_chr(&c) && c == '~') {
						/* <esc>[3~ = delete */
						ev->key = TERM_KEY_DELETE;
						return;
					}
					break;
				case '5':
					if (try_read_chr(&c) && c == '~') {
						/* <esc>[5~ = page up */
						ev->key = TERM_KEY_PAGE_UP;
						return;
					}
					break;
				case '6':
					if (try_read_chr(&c) && c == '~') {
						/* <esc>[6~ = page down */
						ev->key = TERM_KEY_PAGE_DOWN;
						return;
					}
					break;
				}
			} else {
				/* it's just esc */
				ev->key = TERM_KEY_ESC;
				return;
			}
			break;
		case 127:
			/* <DEL> = backspace */
			ev->key = TERM_KEY_BACKSPACE;
			return;
		case '\r':
			/* carriage return = enter */
			ev->key = TERM_KEY_ENTER;
			return;
		case '\011':
			/* <ht>, horizontal tab */
			ev->key = TERM_KEY_TAB;
			return;
		default:
			if (c < 0x20) {
				/* ctrl+<something> */
				ev->key = TERM_KEY_CTRL;
				ev->ch = c + 0x40;
				return;
			} else if (c < 0x7f) {
				/* regular ASCII char */
				ev->key = TERM_KEY_CHAR;
				ev->ch = c;
				return;
			}
			break;
		}
	}
}

static void
term_clear_row(int y)
{
	/* clear the row at y-coordinate y. */
	if (y < 0)
		return;
	printf("\033[%d;H\033[2K", y + 1);
	fflush(stdout);
}

static void
term_event_wait(struct term_event *ev)
{
	/* wait for a terminal event (either resize or keypress). */
	fd_set rfds;
	int rv;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
#if ENABLE_NONPOSIX && defined(SIGWINCH)
	/* do pselect() to wait for SIGWINCH or data on stdin */
	rv = pselect(1, &rfds, NULL, NULL, NULL, &oldmask);
	if (rv < 0) {
		if (errno == EINTR && win_resized) {
			/* got SIGWINCH */
			win_resized = 0;
			ev->type = TERM_EVENT_RESIZE;
		} else {
			die("pselect:");
		}
	} else if (rv) {
		/* data available on stdin */
		ev->type = TERM_EVENT_KEY;
		readkey(ev);
	} else {
		/* ... can this even happen? */
		die("pselect: timeout");
	}
#else
	/* do select() to wait for data on stdin */
	rv = select(1, &rfds, NULL, NULL, NULL);
	if (rv < 0) {
		die("select:");
	} else if (rv) {
		/* data available on stdin */
		ev->type = TERM_EVENT_KEY;
		readkey(ev);
	} else {
		/* ... can this even happen? */
		die("select: timeout");
	}
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */
}

static void
term_init(void)
{
	/* initialize the terminal for use by the other term_* functions. */
#if ENABLE_NONPOSIX && defined(SIGWINCH)
	struct sigaction sa;
	sigset_t mask;
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		die("stdin and stdout must be a terminal");
	if (tcgetattr(STDIN_FILENO, &tio) < 0)
		die("tcgetattr:");
	memcpy(&oldtio, &tio, sizeof(struct termios));

	/* set raw mode */
	tio.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP
			| INLCR | IGNCR | ICRNL | IXON);
	tio.c_oflag &= ~(tcflag_t)OPOST;
	tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
	tio.c_cflag |= (tcflag_t)CS8;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) < 0)
		die("tcsetattr:");
	++term_init_stage;

	/* set non-blocking mode for stdin */
	if ((old_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0)) < 0)
		die("fcntl:");
	if (fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags | O_NONBLOCK) < 0)
		die("fcntl:");
	++term_init_stage;

#if ENABLE_NONPOSIX && defined(SIGWINCH)
	/* set handler for SIGWINCH */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = winch;
	if (sigaction(SIGWINCH, &sa, NULL) < 0)
		die("sigaction:");

	/* block SIGWINCH */
	sigemptyset(&mask);
	sigaddset(&mask, SIGWINCH);
	if (sigprocmask(SIG_BLOCK, &mask, &oldmask) < 0)
		die("sigprocmask:");
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */

	write(STDOUT_FILENO, "\033[2J", 4);
}

static void
term_print(int x, int y, const char *color, const char *str)
{
	/*
	 * clear the row at y-coordinate y and print the string str at
	 * the location (x, y).
	 */
	if (x < 0 || y < 0)
		return;
	printf("\033[%d;%dH\033[2K", y + 1, x + 1);
	if (color)
		fputs(color, stdout);
	fputs(str, stdout);
	if (color)
		fputs(COLOR_RESET, stdout);
	fflush(stdout);
}

static void
term_printf(int x, int y, const char *color, const char *fmt, ...)
{
	/* same as term_print, but printf. */
	va_list ap;

	if (x < 0 || y < 0)
		return;
	printf("\033[%d;%dH\033[2K", y + 1, x + 1);
	if (color)
		fputs(color, stdout);

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	if (color)
		fputs(COLOR_RESET, stdout);
	fflush(stdout);
}

static void
term_set_cursor(int x, int y)
{
	/* set the cursor to the location (x, y). */
	if (x < 0 || y < 0)
		return;
	printf("\033[%d;%dH", y + 1, x + 1);
	fflush(stdout);
}

static void
term_shutdown(void)
{
	/* avoid infinite recursion with die() if something fails */
	int stage = term_init_stage;
	term_init_stage = 0;

	if (stage && tcsetattr(STDIN_FILENO, TCSANOW, &oldtio) < 0)
		die("tcsetattr:");

	if (stage > 1 && fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags) < 0)
		die("fcntl:");

	write(STDOUT_FILENO, "\033[2J\033[;H", 8);
}

static int
term_size(int *w, int *h)
{
	/*
	 * get the terminal's size, storing the width in *w and height in *h.
	 * if getting the size fails, return -1; otherwise, return 0.
	 */
	fd_set rfds;
	struct timeval timeout;

	char buf[16];
	char c = 0;
	size_t i = 0;

#if ENABLE_NONPOSIX && defined(TIOCGWINSZ)
	/* if we have TIOCGWINSZ, find the window size with it */
	struct winsize sz;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &sz) == 0 &&
			sz.ws_col && sz.ws_row) {
		*w = sz.ws_col;
		*h = sz.ws_row;
		return 0;
	}
#endif /* ENABLE_NONPOSIX && defined(TIOCGWINSZ) */

	/*
	 * if that failed or we don't have it, fall back to using
	 * escape sequences
	 */
	if (write(STDOUT_FILENO, "\033[9999;9999H\033[6n", 16) != 16)
		return -1;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	timeout.tv_sec = 0;
	timeout.tv_usec = RESIZE_FALLBACK_MS * 1000;

	if (select(1, &rfds, NULL, NULL, &timeout) < 1)
		return -1;

	for (; i < sizeof(buf) && c != 'R'; ++i) {
		if (read(STDIN_FILENO, &c, 1) < 0)
			return -1;
		buf[i] = c;
	}
	if (i < 2 || i == sizeof(buf))
		return -1;
	buf[i] = '\0';
	if (sscanf(buf, "\033[%d;%dR", h, w) != 2)
		return -1;
	return 0;
}

static int
try_read_chr(char *c)
{
	/*
	 * try to read a character from stdin.
	 * if there's no data available on stdin, return 0.
	 * if there's data available, store the character in *c and return 1.
	 *
	 * requires stdin to be in non-blocking mode.
	 */
	if (read(STDIN_FILENO, c, 1) < 0) {
		if (errno == EAGAIN)
			return 0;
		else
			die("read:");
	}
	return 1;
}

#if ENABLE_NONPOSIX && defined(SIGWINCH)
static void
winch(int unused)
{
	(void)unused;
	win_resized = 1;
}
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */

/*
 * ============================================================================
 * strings
 */
static size_t
count_tabs(const char *s, size_t l)
{
	size_t tabs = 0;
	size_t i = 0;
	for (; i < l; ++i)
		if (s[i] == '\t')
			++tabs;
	return tabs;
}

/*
 * ============================================================================
 * rows
 */
static void
row_insertchar(struct row *row, char c, size_t index, size_t size_increment)
{
	/*
	 * insert the character c into a row at the index index.
	 * if the string is too small to hold the extra character,
	 * increase its size by size_increment.
	 */
	if (row->len + 1 >= row->size) {
		/* if the rowing is too small, increase its size */
		row->size += size_increment;
		row->s = erealloc(row->s, row->size);
	}

	if (index > row->len)
		index = row->len;

	if (row->len == index) {
		/* if we need to insert at the end, simply do that */
		row->s[index] = c;
		row->s[index + 1] = '\0';
		++row->len;
	} else {
		/*
		 * if we need to insert elsewhere, move the portion
		 * of the rowing beginning from the index forwards
		 * to make room for the character
		 */
		memmove(row->s + index + 1, row->s + index,
				row->len - index + 1);
		row->s[index] = c;
		++row->len;
	}

	if (c == '\t')
		++row->tabs;
}

static void
row_removechar(struct row *row, size_t index)
{
	/* remove the character located at index index from a row. */
	char c;
	if (row->len == 0)
		return;

	if (index > row->len)
		index = row->len;

	c = row->s[index];
	if (row->len - 1 == index) {
		/* if we need to remove at the end, just do that */
		row->s[index] = '\0';
	} else {
		/*
		 * if we need to remove elsewhere, shift the portion
		 * of the string beginning from (index + 1) backwards
		 */
		memmove(row->s + index, row->s + index + 1, row->len - index);
	}
	--row->len;

	/*
	 * NOTE: might cause an integer underflow if there's a bug that
	 * i didn't notice but i think it's fine
	 */
	if (c == '\t')
		--row->tabs;
}

/*
 * ============================================================================
 * buffer management
 */
static void
buf_char_insert(struct buf *buf, size_t elem, char c, size_t index)
{
	/*
	 * insert a character into a specific element of a buffer,
	 * creating it if it doesn't exist.
	 */
	if (elem >= buf->size) {
		size_t newsize = elem;
		if (newsize % BUF_SIZE_INCREMENT == 0)
			++newsize;
		buf_resize(buf, ROUNDUPTO(newsize, BUF_SIZE_INCREMENT));
	}
	if (elem >= buf->len)
		buf->len = elem + 1;
	if (!buf->b[elem]) {
		buf->b[elem] = emalloc(sizeof(struct row));
		buf->b[elem]->s = emalloc(INITIAL_ROW_SIZE);
		buf->b[elem]->s[0] = c;
		buf->b[elem]->s[1] = '\0';
		buf->b[elem]->len = 1;
		buf->b[elem]->size = INITIAL_ROW_SIZE;
		if (c == '\t')
			buf->b[elem]->tabs = 1;
		else
			buf->b[elem]->tabs = 0;
	} else {
		row_insertchar(buf->b[elem], c, index, ROW_SIZE_INCREMENT);
	}
}

static void
buf_char_remove(struct buf *buf, size_t elem, size_t index)
{
	/* remove a character from a specific element of a buffer. */
	if (elem < buf->size && buf->b[elem])
		row_removechar(buf->b[elem], index);
}

static void
buf_create(struct buf *buf, size_t size)
{
	/* create a new buffer. */
	buf->b = ecalloc(size, sizeof(struct row *));
	buf->len = 1;
	buf->size = size;
}

static size_t
buf_elem_len(struct buf *buf, size_t elem)
{
	/*
	 * returns the length of an element of a buffer, or 0 if it
	 * doesn't exist.
	 */
	return (buf->b[elem]) ? buf->b[elem]->len : 0;
}

static size_t
buf_elem_visual_len(struct buf *buf, size_t elem)
{
	/*
	 * returns the length of an element of a buffer, or 0 if it
	 * doesn't exist. tabs are TAB_WIDTH characters long instead of 1.
	 */
	if (!buf->b[elem])
		return 0;

	if (!buf->b[elem]->tabs)
		return buf->b[elem]->len;
	else
		return (buf->b[elem]->len - buf->b[elem]->tabs) +
			(buf->b[elem]->tabs * TAB_WIDTH);
}

static void
buf_free(const struct buf *buf)
{
	/* free a buffer and all of its elements. */
	size_t i = 0;
	for (; i < buf->len; ++i) {
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
	/* resize a buffer. */
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
			if (buf->len > size) {
				buf->len = size - 1;
				while (buf->len && !buf->b[buf->len])
					--buf->len;
			}
		}

		buf->b = ereallocarray(buf->b, size, sizeof(struct row *));
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
buf_shift_down(struct buf *buf, size_t start_index, size_t size_increment)
{
	/*
	 * shift every element of a buffer starting from the index
	 * start_index (included) downwards by 1 element.
	 *
	 * if the buffer is too small, its size is increased by
	 * size_increment elements.
	 *
	 * the newly created element has an undefined value.
	 */
	if (buf->len + 1 > buf->size)
		buf_resize(buf, buf->size + size_increment);

	memmove(&buf->b[start_index + 1], &buf->b[start_index],
			(buf->len - start_index) * sizeof(struct row *));
	++buf->len;
}

static void
buf_shift_up(struct buf *buf, size_t start_index)
{
	/*
	 * shift every element of a buffer starting from the index
	 * start_index (included) upwards by 1 element.
	 *
	 * the element at (start_index - 1) is overwritten.
	 * the previously last element now has a value of NULL.
	 * if start_index is 0, the behaviour is the same as if it was 1.
	 */
	memmove(&buf->b[start_index - 1], &buf->b[start_index],
			(buf->len - start_index) * sizeof(struct row *));
	buf->b[--buf->len] = NULL;
}

/*
 * ============================================================================
 * buffer file operations
 */
static int
buf_from_file(struct buf *buf, const char *filename)
{
	/* create a buffer and read the contents of a file into it */
	char *s;
	size_t l, n, elem = 0;
	FILE *f = fopen(filename, "r");
	if (!f)
		return -1;

	buf_create(buf, FILE_BUFFER_ROWS);

	for (errno = 0; ; ++elem) {
		if (elem >= buf->size) {
			size_t newsize = elem;
			if (newsize % FILE_BUF_SIZE_INCR == 0)
				++newsize;
			buf_resize(buf, ROUNDUPTO(newsize,
						FILE_BUF_SIZE_INCR));
		}
		s = NULL;
		n = 0;
		if (getline(&s, &n, f) < 0) {
			if (errno) {
				fclose(f);
				die("getline:");
			} else {
				break;
			}
		}
		l = strlen(s);
		if (l && s[l - 1] == '\n')
			s[--l] = '\0';
		buf->b[elem] = emalloc(sizeof(struct row));
		buf->b[elem]->s = s;
		buf->b[elem]->size = n;
		buf->b[elem]->len = l;
		buf->b[elem]->tabs = count_tabs(s, l);
	}
	buf->len = elem;
	fclose(f);
	return 0;
}

static int
buf_write(const struct buf *buf, const char *filename, int overwrite)
{
	/* write the contents of a buffer to a file. */
	struct iovec iov[IOV_SIZE];
	int fd;
	char newline = '\n';
	int iovcnt = 0; /* int since writev() takes an int for iovcnt */
	size_t i = 0;

	if (overwrite)
		fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC,
				NEW_FILE_MODE);
	else
		fd = open(filename, O_WRONLY | O_CREAT | O_EXCL,
				NEW_FILE_MODE);
	if (fd < 0)
		return -1;

	for (; i < buf->len; ++i) {
		if (buf->b[i]) {
			if (iov_write(iov, &iovcnt, IOV_SIZE, fd,
					buf->b[i]->s, buf->b[i]->len) < 0)
				return -1;
			if (iov_write(iov, &iovcnt, IOV_SIZE, fd,
					&newline, 1) < 0)
				return -1;
		} else if (iov_write(iov, &iovcnt, IOV_SIZE, fd,
					&newline, 1) < 0) {
			return -1;
		}
	}
	if (iovcnt && writev(fd, iov, iovcnt) < 0)
		return -1;
	return close(fd);
}

static int
iov_write(struct iovec *iov, int *iovcnt, size_t iov_size, int writefd,
		char *str, size_t len)
{
	/*
	 * add an entry to an array of iovec structures, or flush the
	 * data to a file if there's no space remaining in the array.
	 */
	if ((size_t)*iovcnt >= iov_size) {
		if (writev(writefd, iov, *iovcnt) < 0)
			return -1;
		*iovcnt = 0;
	}
	iov[*iovcnt].iov_base = str;
	iov[(*iovcnt)++].iov_len = len;
	return 0;
}

/*
 * ============================================================================
 * movement
 */
static void
cursor_fix_xpos(struct state *st)
{
	/* begin searching for valid tx values on the row starting from x */
	size_t i = 0;
	int valid_tx = 0;
	int found = 0;
	if (st->x == 0) {
		st->tx = 0;
		return;
	}
	for (; i < st->buf.b[st->y]->len; ++i) {
		if (st->buf.b[st->y]->s[i] == '\t')
			valid_tx += 8;
		else
			++valid_tx;
		if (valid_tx >= st->tx) {
			found = 1;
			break;
		}
	}

	/*
	 * if we didn't find a valid tx value, valid_tx will be the visual
	 * length of the row
	 */
	st->x = (int)i + found;
	st->tx = valid_tx;
}

static void
cursor_up(struct state *st)
{
	if (st->y) {
		size_t elen = buf_elem_len(&st->buf, (size_t)--st->y);
		if ((size_t)st->x > elen)
			st->x = (int)elen;
		cursor_fix_xpos(st);
		if (st->ty)
			--st->ty;
		else
			redraw(st, st->y, 0, st->h - 2);
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_down(struct state *st)
{
	if (st->buf.len && (size_t)st->y < st->buf.len - 1) {
		size_t elen = buf_elem_len(&st->buf, (size_t)++st->y);
		if ((size_t)st->x > elen)
			st->x = (int)elen;
		cursor_fix_xpos(st);
		if (st->ty < st->h - 2) {
			++st->ty;
		} else {
			write(STDOUT_FILENO, "\r\n\r\n", 4);
			redraw_row(st, st->y, st->h - 2);
		}
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_right(struct state *st, int stopatlastchar)
{
	size_t l = buf_elem_len(&st->buf, (size_t)st->y);
	if (stopatlastchar && l)
		--l;
	if (st->tx < st->w - 1 && (size_t)st->x < l) {
		if (st->buf.b[st->y]->s[st->x] == '\t')
			st->tx += 8;
		else
			++st->tx;
		++st->x;
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_left(struct state *st)
{
	if (st->x) {
		if (st->buf.b[st->y]->s[--st->x] == '\t')
			st->tx -= 8;
		else
			--st->tx;
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_linestart(struct state *st)
{
	st->x = st->tx = 0;
	term_set_cursor(st->tx, st->ty);
}

static void
cursor_lineend(struct state *st, int stopbeforelastchar)
{
	st->x = (int)buf_elem_len(&st->buf, (size_t)st->y);
	st->tx = (int)buf_elem_visual_len(&st->buf, (size_t)st->y);
	if (stopbeforelastchar && st->x) {
		if (st->buf.b[st->y]->s[--st->x] == '\t')
			st->tx -= 8;
		else
			--st->tx;
	}
	term_set_cursor(st->tx, st->ty);
}

static void
cursor_startnextrow(struct state *st, int stripextranewline)
{
	if (st->buf.len && (size_t)st->y < st->buf.len - 1) {
		++st->y;
		st->x = st->tx = 0;

		if (st->ty < st->h - 2) {
			++st->ty;
		} else {
			if (stripextranewline)
				write(STDOUT_FILENO, "\r\n", 2);
			else
				write(STDOUT_FILENO, "\r\n\r\n", 4);
			redraw_row(st, st->y, st->h - 2);
		}
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_endpreviousrow(struct state *st)
{
	if (st->y) {
		st->x = (int)buf_elem_len(&st->buf, (size_t)--st->y);
		st->tx = (int)buf_elem_visual_len(&st->buf, (size_t)st->y);
		if (st->ty)
			--st->ty;
		else
			redraw(st, st->y, 0, st->h - 2);
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_nonblank(struct state *st)
{
	if (BUF_ELEM_NOTEMPTY(st->buf, st->y)) {
		size_t l = st->buf.b[st->y]->len;
		st->tx = 0;
		for (st->x = 0; st->x < (int)l; ++st->x) {
			if (!isblank(st->buf.b[st->y]->s[st->x]))
				break;
			if (st->buf.b[st->y]->s[st->x] == '\t')
				st->tx += TAB_WIDTH;
			else
				++st->tx;
		}
		if (st->x == (int)l) {
			if (st->buf.b[st->y]->s[--st->x] == '\t')
				st->tx -= 8;
			else
				--st->tx;
		}
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_up_page(struct state *st)
{
	if (st->y - st->ty > 0) {
		/*
		 * now, you might think that this is written rly badly;
		 * this is because it is, i have no idea what i was doing,
		 * i just mimicked neovim's behavior
		 */

		size_t elen;
		int redrawpos = st->y - (st->h - 2) - (st->ty - 1);
		if (redrawpos < 0)
			redrawpos = 0;
		st->y -= st->h - 3;
		if (st->y < st->h - 2)
			st->y = st->h - 2;
		elen = buf_elem_len(&st->buf, (size_t)st->y);
		if ((size_t)st->x > elen)
			st->x = (int)elen;
		cursor_fix_xpos(st);
		st->ty = st->h - 2;
		redraw(st, redrawpos, 0, st->h - 2);
		term_set_cursor(st->tx, st->ty);
	}
}

static void
cursor_down_page(struct state *st)
{
	if (st->buf.len && (size_t)st->y < st->buf.len - 1) {
		/*
		 * this was self-written but i have no idea what
		 * i was doing here either
		 */
		size_t elen;
		st->y += st->h - 3;
		if ((size_t)st->y > st->buf.len - 1)
			st->y = (int)st->buf.len - 1;
		elen = buf_elem_len(&st->buf, (size_t)st->y);
		if ((size_t)st->x > elen)
			st->x = (int)elen;
		cursor_fix_xpos(st);
		st->ty = 0;
		redraw(st, st->y, 0, st->h - 2);
		term_set_cursor(st->tx, st->ty);
	}
}

/*
 * ============================================================================
 * commands
 */
static const char *
cmdarg(const char *cmd)
{
	/*
	 * get the argument portion of a command.
	 * returns NULL if there isn't one.
	 */

	/*
	 * if there's a space and the char afterwards isn't a '\0',
	 * return a pointer to that char
	 */
	const char *p = strchr(cmd, ' ');
	return (p && *(++p)) ? p : NULL;
}

static int
cmdchrcmp(const char *cmd, char c)
{
	/*
	 * check if a command (not counting the argument portion) equals to
	 * a character with an optional bang at the end.
	 *
	 * for example, cmdchrcmp(cmd, 'q') will return true for "q", "q!",
	 * "q " followed by anything, or "q! " followed by anything.
	 */
	size_t end = 1;

	/* if the command doesn't start with c, return false */
	if (cmd[0] != c)
		return 0;

	/* if there's a bang, count it as part of the command */
	if (cmd[end] == '!')
		++end;

	/* if the end of the command isn't a '\0' or a ' ', return false */
	if (cmd[end] != '\0' && cmd[end] != ' ')
		return 0;
	return 1;
}

static int
cmdstrcmp(const char *cmd, const char *s, size_t sl)
{
	/* same as cmdchrcmp(), but compare against a string. */
	size_t end = sl;

	/* if the command doesn't start with s, return false */
	if (strncmp(cmd, s, sl))
		return 0;

	/* if there's a bang, count it as part of the command */
	if (cmd[end] == '!')
		++end;

	/* if the end of the command isn't a '\0' or a ' ', return false */
	if (cmd[end] != '\0' && cmd[end] != ' ')
		return 0;
	return -1;
}

static int
exec_cmd(struct state *st)
{
	/* execute a command. returns 0 on success and -1 on error. */
	if (cmdchrcmp(st->cmd.s, 'q')) {
		/* :q || :q! */
		if (st->cmd.s[1] != '!' && st->modified) {
			term_print(0, st->h - 1, COLOR_RED,
					"buffer modified");
			return -1;
		}
		st->done = 1;
	} else if (cmdchrcmp(st->cmd.s, 'w') || cmdstrcmp(st->cmd.s, "wq",
				2)) {
		/* :w || :w! || :wq || :wq! */
		const char *arg = cmdarg(st->cmd.s);
		const char *name = (arg) ? arg : st->name;
		int bang = (st->cmd.s[1] == '!' || (st->cmd.s[1] == 'q' &&
					st->cmd.s[2] == '!'));

		if (arg && !st->name) {
			st->name = estrdup(arg);
			st->name_needs_free = 1;
		}
		if (name) {
			if (buf_write(&st->buf, name,
						bang || st->written) < 0) {
				if (errno == EEXIST) {
					term_print(0, st->h - 1,
							COLOR_RED,
							"file exists (add ! "
							"to override)");
				} else {
					term_printf(0, st->h - 1,
							COLOR_RED,
							"writing to file "
							"failed: %s",
							strerror(errno));
				}
				return -1;
			}
			st->modified = 0;
			st->written = 1;
		} else {
			term_print(0, st->h - 1, COLOR_RED,
					"no file name specified");
			return -1;
		}
		if (st->cmd.s[1] == 'q')
			st->done = 1;
	}
	return 0;
}

/*
 * ============================================================================
 * helper functions
 */
static void
draw_row(int y, struct row *s)
{
	if (y < 0)
		return;
	term_clear_row(y);
	printf("\033[%d;1H\033[2K", y + 1);
	if (s->tabs) {
		size_t i = 0;
		size_t tx = 0;
		size_t maxtx = (s->len - s->tabs) + (s->tabs * TAB_WIDTH);
		for (; i < s->len && tx < maxtx; ++i) {
			if (s->s[i] == '\t') {
				fputs(TAB_WIDTH_CHARS, stdout);
				tx += TAB_WIDTH;
			} else {
				putchar(s->s[i]);
				++tx;
			}
		}
	} else {
		fputs(s->s, stdout);
	}
	fflush(stdout);
}

static void
insert_newline(struct state *st)
{
	if (BUF_ELEM_NOTEMPTY(st->buf, st->y) &&
			(size_t)st->x < st->buf.b[st->y]->len) {
		/*
		 * there is text on this row and the cursor
		 * is located inside some text, shift the
		 * text after the cursor down to the next row
		 */

		/* length of new row */
		size_t newlen = st->buf.b[st->y]->len - (size_t)st->x;

		/* size of new row */
		size_t newsize = newlen;

		/* amount of tabs in new row */
		size_t newtabs;

		if (newsize % ROW_SIZE_INCREMENT == 0)
			++newsize;
		newsize = ROUNDUPTO(newsize, ROW_SIZE_INCREMENT);

		/* shift down all rows below cursor */
		buf_shift_down(&st->buf, (size_t)(st->y + 1),
				BUF_SIZE_INCREMENT);

		/* create new row in the newly freed space */
		st->buf.b[st->y + 1] = emalloc(sizeof(struct row));
		st->buf.b[st->y + 1]->s = emalloc(newsize);

		/*
		 * copy over the portion of the old row after
		 * the cursor
		 */
		memcpy(st->buf.b[st->y + 1]->s, st->buf.b[st->y]->s + st->x,
				newlen);
		st->buf.b[st->y + 1]->s[newlen] = '\0';
		st->buf.b[st->y + 1]->len = newlen;
		st->buf.b[st->y + 1]->size = newsize;
		st->buf.b[st->y + 1]->tabs = newtabs = count_tabs(
				st->buf.b[st->y + 1]->s, newlen);

		/* cut off the old row at the cursor */
		st->buf.b[st->y]->s[st->x] = '\0';
		st->buf.b[st->y]->len = (size_t)st->x;
		st->buf.b[st->y]->tabs -= newtabs;

		/* redraw screen */
		redraw(st, st->y, st->ty, st->h - 2);
	} else if ((size_t)st->y < st->buf.len - 1) {
		/*
		 * there is text after this row and we're either
		 * at the end of the row or this row is empty
		 */
		buf_shift_down(&st->buf, (size_t)(st->y + 1),
				BUF_SIZE_INCREMENT);
		st->buf.b[st->y + 1] = NULL;

		/* redraw screen */
		redraw(st, st->y + 1, st->ty + 1, st->h - 2);
	} else {
		/* there's no text after this row */
		++st->buf.len;
		term_clear_row(st->y + 1);
	}
	cursor_startnextrow(st, 1);
}

static void
redraw(struct state *st, int start_y, int start_ty, int end_ty)
{
	/*
	 * redraw a portion of the screen starting from start_ty and
	 * ending at end_ty (both included).
	 *
	 * the content of the redrawn rows is the buffer element at index
	 * [start_y + how many rows have already been drawn].
	 */
	for (; start_ty <= end_ty; ++start_ty)
		redraw_row(st, start_y++, start_ty);
}

static void
redraw_row(struct state *st, int y, int ty)
{
	if ((size_t)y < st->buf.len) {
		if (BUF_ELEM_NOTEMPTY(st->buf, y))
			draw_row(ty, st->buf.b[y]);
		else
			term_clear_row(ty);
	} else {
		term_print(0, ty, COLOR_DEFAULT, "~");
	}
}

static void
remove_newline(struct state *st)
{
	/* we can assume that (st->x == 0 && st->y) */
	if (BUF_ELEM_NOTEMPTY(st->buf, st->y) && BUF_ELEM_NOTEMPTY(st->buf,
				st->y - 1)) {
		/* stick the current row to the end of the previous row */
		size_t oldlen = st->buf.b[st->y - 1]->len;
		size_t oldvlen = buf_elem_visual_len(&st->buf,
				(size_t)(st->y - 1));
		size_t newlen = oldlen + st->buf.b[st->y]->len;

		if (newlen >= st->buf.b[st->y - 1]->size) {
			/* if the row above is too small, increase its size */
			size_t newsize = newlen;
			if (newsize % ROW_SIZE_INCREMENT == 0)
				++newsize;
			st->buf.b[st->y - 1]->size = ROUNDUPTO(newsize,
					ROW_SIZE_INCREMENT);
			st->buf.b[st->y - 1]->s = erealloc(
					st->buf.b[st->y - 1]->s,
					st->buf.b[st->y - 1]->size);
		}
		memcpy(st->buf.b[st->y - 1]->s + oldlen,
				st->buf.b[st->y]->s,
				st->buf.b[st->y]->len + 1);
		st->buf.b[st->y - 1]->len = newlen;
		if (st->buf.b[st->y]) {
			free(st->buf.b[st->y]->s);
			free(st->buf.b[st->y]);
		}
		st->x = (int)oldlen;
		st->tx = (int)oldvlen;
		buf_shift_up(&st->buf, (size_t)(st->y + 1));
	} else if (BUF_ELEM_NOTEMPTY(st->buf, st->y - 1)) {
		/*
		 * this row is empty
		 * the row above is not empty
		 */
		if (st->buf.b[st->y]) {
			free(st->buf.b[st->y]->s);
			free(st->buf.b[st->y]);
		}
		st->x = (int)st->buf.b[st->y - 1]->len;
		st->tx = (int)buf_elem_visual_len(&st->buf, (size_t)st->y - 1);
		buf_shift_up(&st->buf, (size_t)(st->y + 1));
	} else {
		/*
		 * this row is not empty
		 * the row above is empty
		 */
		if (st->buf.b[st->y - 1]) {
			free(st->buf.b[st->y - 1]->s);
			free(st->buf.b[st->y - 1]);
		}
		buf_shift_up(&st->buf, (size_t)st->y);
	}

	/* don't call cursor_endpreviousrow() to avoid double redraws */
	if (st->ty)
		--st->ty;
	redraw(st, --st->y, st->ty, st->h - 2);
	term_set_cursor(st->tx, st->ty);
}

/*
 * ============================================================================
 * event handling
 */
static void
key_command_line(struct state *st)
{
	/* handle a key event in command-line mode. */
	switch (st->ev.key) {
	case TERM_KEY_ESC:
		/* discard command and return to normal mode */
		st->mode = MODE_NORMAL;
		st->cmd.s[0] = '\0';
		st->cmd.len = 0;
		term_clear_row(st->h - 1);
		st->tx = st->storedtx;
		term_set_cursor(st->tx, st->ty);
		break;
	case TERM_KEY_ARROW_RIGHT:
		/* move cursor right */
		if (st->tx < st->w - 1 && (size_t)(st->tx - 1) <
				st->cmd.len)
			term_set_cursor(++st->tx, st->h - 1);
		break;
	case TERM_KEY_ARROW_LEFT:
		/* move cursor left */
		if (st->tx > 1)
			term_set_cursor(--st->tx, st->h - 1);
		break;
	case TERM_KEY_HOME:
		st->tx = 1;
		term_set_cursor(st->tx, st->h - 1);
		break;
	case TERM_KEY_END:
		st->tx = (int)(st->cmd.len + 1);
		term_set_cursor(st->tx, st->h - 1);
		break;
	case TERM_KEY_DELETE:
		/*
		 * remove char at cursor, if there's some
		 * text on the current row
		 */
		if (st->cmd.len) {
			row_removechar(&st->cmd, (size_t)(st->tx - 1));
			term_printf(0, st->h - 1, COLOR_DEFAULT,
					":%s", st->cmd.s);
			term_set_cursor(st->tx, st->h - 1);
		}
		break;
	case TERM_KEY_BACKSPACE:
		/*
		 * remove char behind cursor, if it's not
		 * at the beginning of the row and there's
		 * some text on the current row
		 */
		if (st->tx > 1 && st->cmd.len) {
			row_removechar(&st->cmd, (size_t)(st->tx - 2));
			term_printf(0, st->h - 1, COLOR_DEFAULT,
					":%s", st->cmd.s);
			term_set_cursor(--st->tx, st->h - 1);
		}
		break;
	case TERM_KEY_ENTER:
		/* execute command and return to normal mode */
		if (exec_cmd(st) >= 0)
			term_clear_row(st->h - 1);
		st->mode = MODE_NORMAL;
		st->cmd.s[0] = '\0';
		st->cmd.len = 0;
		st->tx = st->storedtx;
		term_set_cursor(st->tx, st->y);
		break;
	case TERM_KEY_CHAR:
		/* regular key */
		if (st->tx && st->tx < st->w - 1) {
			row_insertchar(&st->cmd, st->ev.ch,
					(size_t)(st->tx - 1),
					CMD_SIZE_INCREMENT);
			term_printf(0, st->h - 1, COLOR_DEFAULT,
					":%s", st->cmd.s);
			term_set_cursor(++st->tx, st->h - 1);
		}
		break;
	default:
		break;
	}
}

static void
key_insert(struct state *st)
{
	/* handle a key event in insert mode. */
	switch (st->ev.key) {
	case TERM_KEY_ESC:
		/* go into normal mode */
		st->mode = MODE_NORMAL;
		term_clear_row(st->h - 1);
		term_set_cursor(st->tx, st->ty);
		break;
	case TERM_KEY_ARROW_UP:
		cursor_up(st);
		break;
	case TERM_KEY_ARROW_DOWN:
		cursor_down(st);
		break;
	case TERM_KEY_ARROW_RIGHT:
		cursor_right(st, 1);
		break;
	case TERM_KEY_ARROW_LEFT:
		cursor_left(st);
		break;
	case TERM_KEY_HOME:
		cursor_linestart(st);
		break;
	case TERM_KEY_END:
		cursor_lineend(st, 1);
		break;
	case TERM_KEY_DELETE:
		/*
		 * remove char at cursor, if there's some
		 * text on the current row
		 */
		if (BUF_ELEM_NOTEMPTY(st->buf, st->y)) {
			st->modified = 1;
			buf_char_remove(&st->buf, (size_t)st->y,
					(size_t)st->x);
			draw_row(st->ty, st->buf.b[st->y]);
			term_set_cursor(st->tx, st->ty);
		}
		break;
	case TERM_KEY_PAGE_UP:
		cursor_up_page(st);
		break;
	case TERM_KEY_PAGE_DOWN:
		cursor_down_page(st);
		break;
	case TERM_KEY_BACKSPACE:
		/*
		 * remove char behind cursor, if it's not
		 * at the beginning of the row and there's
		 * some text on the current row
		 */
		if (st->x && BUF_ELEM_NOTEMPTY(st->buf, st->y)) {
			if (st->buf.b[st->y]->s[--st->x] == '\t')
				st->tx -= TAB_WIDTH;
			else
				--st->tx;
			st->modified = 1;
			buf_char_remove(&st->buf, (size_t)st->y,
					(size_t)st->x);
			draw_row(st->ty, st->buf.b[st->y]);
			term_set_cursor(st->tx, st->ty);
		} else if (st->x == 0 && st->y) {
			st->modified = 1;
			remove_newline(st);
		}
		break;
	case TERM_KEY_ENTER:
		st->modified = 1;
		insert_newline(st);
		break;
	case TERM_KEY_TAB:
		if (st->tx < st->w - TAB_WIDTH) {
			st->modified = 1;
			st->tx += 8;
			buf_char_insert(&st->buf, (size_t)st->y, '\t',
					(size_t)st->x++);
			draw_row(st->ty, st->buf.b[st->y]);
			term_set_cursor(st->tx, st->ty);
		}
		break;
	case TERM_KEY_CHAR:
		/* regular key */
		if (st->tx < st->w - 1) {
			st->modified = 1;
			buf_char_insert(&st->buf, (size_t)st->y, st->ev.ch,
					(size_t)st->x++);
			draw_row(st->ty, st->buf.b[st->y]);
			term_set_cursor(++st->tx, st->ty);
		}
		break;
	default:
		break;
	}
}

static void
key_normal(struct state *st)
{
	/* handle a key event in normal mode. */
	switch (st->ev.key) {
	case TERM_KEY_ARROW_UP:
		cursor_up(st);
		break;
	case TERM_KEY_ARROW_DOWN:
		cursor_down(st);
		break;
	case TERM_KEY_ARROW_RIGHT:
		cursor_right(st, 1);
		break;
	case TERM_KEY_ARROW_LEFT:
		cursor_left(st);
		break;
	case TERM_KEY_HOME:
		cursor_linestart(st);
		break;
	case TERM_KEY_END:
		cursor_lineend(st, 1);
		break;
	case TERM_KEY_INSERT:
		st->mode = MODE_INSERT;
		break;
	case TERM_KEY_DELETE:
		/*
		 * remove char at cursor, if there's some
		 * text on the current row
		 */
		if (BUF_ELEM_NOTEMPTY(st->buf, st->y)) {
			st->modified = 1;
			buf_char_remove(&st->buf, (size_t)st->y,
					(size_t)st->x);
			draw_row(st->ty, st->buf.b[st->y]);
			term_set_cursor(st->tx, st->ty);
		}
		break;
	case TERM_KEY_PAGE_UP:
		cursor_up_page(st);
		break;
	case TERM_KEY_PAGE_DOWN:
		cursor_down_page(st);
		break;
	case TERM_KEY_BACKSPACE:
		/* move to previous char */
		if (st->x == 0 && st->y)
			cursor_endpreviousrow(st);
		else
			cursor_left(st);
		break;
	case TERM_KEY_ENTER:
		cursor_startnextrow(st, 0);
		break;
	case TERM_KEY_CTRL:
		switch (st->ev.ch) {
		case 'L':
			/* clear and redraw screen */
			resized(st);
			break;
		case 'b':
			/* page up */
			cursor_up_page(st);
			break;
		case 'f':
			/* page down */
			cursor_down_page(st);
			break;
		}
		break;
	case TERM_KEY_CHAR:
		switch (st->ev.ch) {
		case 'h':
			cursor_left(st);
			break;
		case 'j':
			cursor_down(st);
			break;
		case 'k':
			cursor_up(st);
			break;
		case 'l':
			cursor_right(st, 1);
			break;
		case '0':
			cursor_linestart(st);
			break;
		case '$':
			cursor_lineend(st, 1);
			break;
		case '^':
			cursor_nonblank(st);
			break;
		case 'i':
			st->mode = MODE_INSERT;
			break;
		case 'I':
			cursor_linestart(st);
			st->mode = MODE_INSERT;
			break;
		case 'a':
			cursor_right(st, 0);
			st->mode = MODE_INSERT;
			break;
		case 'A':
			cursor_lineend(st, 0);
			st->mode = MODE_INSERT;
			break;
		case 'o':
			cursor_lineend(st, 0);
			st->modified = 1;
			insert_newline(st);
			st->mode = MODE_INSERT;
			break;
		case 'O':
			cursor_endpreviousrow(st);
			st->modified = 1;
			insert_newline(st);
			st->mode = MODE_INSERT;
			break;
		case 'x':
			/*
			 * remove char at cursor, if there's some
			 * text on the current row
			 */
			if (BUF_ELEM_NOTEMPTY(st->buf, st->y)) {
				st->modified = 1;
				buf_char_remove(&st->buf, (size_t)st->y,
						(size_t)st->x);
				draw_row(st->ty, st->buf.b[st->y]);
				term_set_cursor(st->tx, st->ty);
			}
			break;
		case ':':
			st->mode = MODE_COMMAND_LINE;
			st->storedtx = st->tx;
			st->tx = 1;
			term_print(0, st->h - 1, COLOR_DEFAULT, ":");
			term_set_cursor(st->tx, st->h - 1);
			break;
		}
		break;
	default:
		break;
	}
}

static void
resized(struct state *st)
{
	/* fetch new terminal size */
	if (term_size(&st->w, &st->h) < 0) {
		st->w = FALLBACK_WIDTH;
		st->h = FALLBACK_HEIGHT;
	}
	if (st->h < 2)
		die("terminal height too low");

	/* clear and redraw screen */
	write(STDOUT_FILENO, "\033[2J", 4);
	redraw(st, (st->y > st->h - 2) ? st->y - (st->h - 2) : 0,
			0, st->h - 2);

	/* set new cursor position on-screen correctly */
	if (st->x > st->w - 2)
		st->x = st->w - 2;
	if (st->tx > st->w - 2)
		st->tx = st->w - 2;

	if (st->ty < st->y && st->y <= st->h - 2)
		st->ty = st->y;
	else if (st->y > st->h - 2)
		st->ty = st->h - 2;

	term_set_cursor(st->tx, st->ty);
}

/*
 * ============================================================================
 * main program loop
 */
static void
run(int argc, char *argv[])
{
	/* main program loop. */
	struct state st;

	/* initialize state */
	if (argc > 1 && access(argv[1], F_OK) == 0)
		/* file already exists, open it */
		buf_from_file(&st.buf, argv[1]);
	else
		/* file not specified or doesn't exist */
		buf_create(&st.buf, INITIAL_BUFFER_ROWS);

	st.cmd.s = emalloc(INITIAL_CMD_SIZE);
	st.cmd.s[0] = '\0';
	st.cmd.len = 0;
	st.cmd.size = INITIAL_ROW_SIZE;

	st.x = st.y = st.ty = st.storedtx = 0;
	st.mode = MODE_NORMAL;
	st.name = (argc > 1) ? argv[1] : NULL;
	st.name_needs_free = st.modified = st.written = st.done = 0;

	/* get terminal size */
	if (term_size(&st.w, &st.h) < 0) {
		st.w = FALLBACK_WIDTH;
		st.h = FALLBACK_HEIGHT;
	}
	if (st.h < 2)
		die("terminal height too low");

	redraw(&st, 0, 0, st.h - 2);
	term_set_cursor(0, 0);

	/* main loop */
	while (!st.done) {
		term_event_wait(&st.ev);

		switch (st.ev.type) {
#if ENABLE_NONPOSIX && defined(SIGWINCH)
		case TERM_EVENT_RESIZE:
			resized(&st);
			break;
#endif /* ENABLE_NONPOSIX && defined(SIGWINCH) */
		case TERM_EVENT_KEY:
			if (st.mode == MODE_COMMAND_LINE)
				key_command_line(&st);
			else if (st.mode == MODE_INSERT)
				key_insert(&st);
			else if (st.mode == MODE_NORMAL)
				key_normal(&st);
			break;
		}
	}

	if (st.name_needs_free)
		free(st.name);
	free(st.cmd.s);
	buf_free(&st.buf);
}

/*
 * ============================================================================
 * main()
 */
int
main(int argc, char *argv[])
{
	if (argc)
		argv0 = argv[0];

#if ENABLE_NONPOSIX && ENABLE_PLEDGE
	if (pledge("stdio rpath wpath cpath tty", NULL) < 0)
		die("pledge:");
#endif /* ENABLE_NONPOSIX && ENABLE_PLEDGE */

	term_init();
	run(argc, argv);
	term_shutdown();
	return 0;
}
