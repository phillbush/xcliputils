PROG = xclipd xclipin xclipout xclipowner
OBJS = ${PROG:=.o} ctrlsel.o util.o
HEAD = ctrlsel.h util.h
SRCS = ${OBJS:.o=.c} ${HEAD}
MANS = xclipd.1

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

DEFS = -D_POSIX_C_SOURCE=200809L -DGNU_SOURCE -D_BSD_SOURCE
INCS = -I${LOCALINC} -I${X11INC}
LIBS = -L${LOCALLIB} -L${X11LIB} -lX11 -lXfixes

all: ${PROG}

${PROG}: ${@:=.o} ctrlsel.o util.o
	${CC} -o $@ ${@:=.o} ctrlsel.o util.o ${LIBS} ${LDFLAGS}

xclipd: xclipd.o
xclipin: xclipin.o
xclipout: xclipout.o
xclipcopy: xclipcopy.o
xclipowner: xclipowner.o

${OBJS}: ctrlsel.h util.h

.c.o:
	${CC} -std=c99 -pedantic ${DEFS} ${INCS} ${CFLAGS} ${CPPFLAGS} -c $<

README: ${MANS}
	man -l ${MANS} | sed 's/.//g' >README

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	install -d ${DESTDIR}${PREFIX}/bin
	install -d ${DESTDIR}${MANPREFIX}/man1
	for i in ${PROG} ; do install -m 755 "$$i" "${DESTDIR}${PREFIX}/bin/$$i" ; done
	install -m 644 ${MANS} ${DESTDIR}${MANPREFIX}/man1/${MANS}

uninstall:
	for i in ${PROG} ; do rm "${DESTDIR}${PREFIX}/bin/$$i" ; done
	rm ${DESTDIR}${MANPREFIX}/man1/${MANS}

.PHONY: all tags clean install uninstall
