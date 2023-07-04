PROG = xclipd xclipin xclipout xclipowner
OBJS = ${PROG:=.o} ctrlsel.o util.o
SRCS = ${OBJS:.o=.c}
MAN  = xclipd.1

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

DEFS = -D_POSIX_C_SOURCE=200809L -DGNU_SOURCE -D_BSD_SOURCE
INCS = -I${LOCALINC} -I${X11INC}
LIBS = -L${LOCALLIB} -L${X11LIB} -lXcursor -lX11

bindir = ${DESTDIR}${PREFIX}/bin
mandir = ${DESTDIR}${MANPREFIX}/man1

all: ${PROG}

xclipd xclipowner: ${@:=.o} ctrlsel.o util.o
	${CC} -o $@ ${@:=.o} ctrlsel.o util.o ${LIBS} -lXfixes ${LDFLAGS}

xclipin xclipout: ${@:=.o} ctrlsel.o util.o
	${CC} -o $@ ${@:=.o} ctrlsel.o util.o ${LIBS} ${LDFLAGS}

.c.o:
	${CC} -std=c99 -pedantic ${DEFS} ${INCS} ${CFLAGS} ${CPPFLAGS} -o $@ -c $<

${OBJS}: ctrlsel.h util.h

README: ${MAN}
	mandoc -I os=UNIX -T ascii ${MAN} | col -b | expand -t 8 >README

tags: ${SRCS}
	ctags ${SRCS}

lint: ${SRCS}
	-mandoc -T lint -W warning ${MAN}
	-clang-tidy ${SRCS} -- -std=c99 ${DEFS} ${INCS} ${CPPFLAGS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	mkdir -p ${bindir}
	mkdir -p ${mandir}
	for i in ${PROG} ; do install -m 755 "$$i" "${bindir}/$$i" ; done
	install -m 644 ${MAN} ${mandir}/${MAN}

uninstall:
	-for i in ${PROG} ; do rm "${bindir}/$$i" ; done
	-rm ${mandir}/${MAN}

.PHONY: all tags clean install uninstall
