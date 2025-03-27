SEL_PROGS = xselin xselout xselowner xselwatch
CLIP_PROGS = xclipin xclipout xclipowner xclipwatch
PROGS = ${SEL_PROGS} ${CLIP_PROGS} xclipd

SHARE_OBJS = control/selection.o util.o
PROG_OBJS = ${PROGS:=.o}
CLIP_OBJS = ${CLIP_PROGS:=.o}
SEL_OBJS = ${SEL_PROGS:=.o}
OBJS = ${PROG_OBJS} ${SHARE_OBJS}

SRCS = ${CLIP_OBJS:.o=.c} ${SHARE_OBJS:.o=.c} xclipd.c
MAN = xcliputils.1

DEBUG_FLAGS = \
	-g -O0 -DDEBUG -Wall -Wextra -Wpedantic

PROG_CPPFLAGS = \
	-I. -I/usr/local/include -I/usr/X11R6/include \
	-D_POSIX_C_SOURCE=202405L ${CPPFLAGS}

PROG_CFLAGS = \
	-std=c99 -pedantic \
	${PROG_CPPFLAGS} \
	${CFLAGS} ${DEBUG_FLAGS}

PROG_LDFLAGS = \
	-L/usr/local/lib -L/usr/X11R6/lib -lX11 -lXfixes \
	${LDFLAGS} ${LDLIBS} ${DEBUG_FLAGS}

all: ${PROGS}

${SHARE_OBJS}: ${@:.o=.h}

${PROGS}: ${@:=.o} ${SHARE_OBJS}
	${CC} -o $@ ${@:=.o} ${SHARE_OBJS} ${PROG_LDFLAGS}

${PROG_OBJS}: control/selection.h util.h
${SEL_OBJS}: ${@:xsel%.o=xclip%.c}
	${CC} ${PROG_CFLAGS} '-DSELECTION="PRIMARY"' -o $@ -c ${@:xsel%.o=xclip%.c}
${CLIP_OBJS}: ${@:.o=.c}
	${CC} ${PROG_CFLAGS} '-DSELECTION="CLIPBOARD"' -o $@ -c ${@:.o=.c}
.c.o:
	${CC} ${PROG_CFLAGS} -o $@ -c $<

README: ${MAN}
	mandoc -I os=UNIX -T utf8 ${MAN} | awk '{while(sub(/.\b/,""))}1' >README

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f ${OBJS} ${PROGS} ${PROGS:=.core} tags

.PHONY: all clean
