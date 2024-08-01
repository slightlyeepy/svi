.POSIX:

CC      = cc
SRC     = svi.c

PREFIX  = /usr/local

WFLAGS  = -Wall -Wextra -Wdeclaration-after-statement -Wshadow -Wpointer-arith -Wcast-align -Wcast-qual -Wwrite-strings -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wnested-externs -Winline -Wconversion -Wstrict-prototypes -Wdeprecated

CFLAGS  = -std=c89 -pedantic -Os -g -Werror ${WFLAGS}

svi: ${SRC}
	${CC} ${CFLAGS} -o svi ${SRC}
install: svi
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f svi ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/svi
uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/svi
clean:
	rm -f svi *.o
