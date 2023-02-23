#include <sys/stat.h>
#include <sys/mman.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "util.h"
#include "ctrlsel.h"

#define CLIPBOARD       "CLIPBOARD"

static void
usage(void)
{
	fprintf(stderr, "usage: xclipin [-pw] [-t target] [file]\n");
	exit(EXIT_FAILURE);
}

static int
fillbuffer(int fd, void *buf, size_t *size)
{
	ssize_t got;
	size_t left = *size;

	while (left != 0) {
		if ((got = read(fd, buf, left)) == -1) {
			if (errno == EINTR)
				continue;
			warn("read");
			return -1;
		}
		if (got == 0)
			break;
		buf = (char *)buf + got;
		left -= got;
	}
	*size -= left;
	return 0;
}

static int
xclipin(Atom selection, char *targetstr, unsigned char *data, size_t size, int wflag)
{
	struct CtrlSelContext context;
	struct CtrlSelTarget target;
	XEvent xev;
	Display *display = NULL;
	Window window = None;
	Atom targetatom = XA_STRING;
	int retval = EXIT_FAILURE;
	int success, status;

	if (!xinit(&display, &window))
		goto error;
#if __OpenBSD__
	if (pledge((wflag ? "stdio" : "stdio proc"), NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
	if (selection == None && (selection = XInternAtom(display, CLIPBOARD, False)) == None) {
		warnx("could not intern atom");
		goto error;
	}
	if (targetstr != NULL && (targetatom = XInternAtom(display, targetstr, False)) == None) {
		warnx("could not intern atom");
		goto error;
	}
	if (!wflag)
		if (xfork() == -1 || xfork() == -1)     /* double fork */
			goto error;
	ctrlsel_filltarget(targetatom, targetatom, 8, data, size, &target);
	success = ctrlsel_setowner(
		display,
		window,
		selection,
		CurrentTime,
		0,
		&target,
		1,
		&context
	);
	if (!success)
		goto error;
	for (;;) {
		(void)XNextEvent(display, &xev);
		status = ctrlsel_send(&context, &xev);
		if (status == CTRLSEL_LOST || status == CTRLSEL_ERROR) {
			break;
		}
	}
	ctrlsel_disown(&context);
	if (status != CTRLSEL_ERROR)
		retval = EXIT_SUCCESS;
error:
	xclose(display, window);
	return retval;
}

int
main(int argc, char *argv[])
{
	struct stat sb;
	Atom selection = None;
	int fd = STDIN_FILENO;
	int wflag = 0;
	int mapped = 1;
	int retval = EXIT_FAILURE;
	int ch;
	size_t size;
	char *targetstr = NULL;
	unsigned char *data = NULL;

#if __OpenBSD__
	if (pledge("unix stdio rpath proc", NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
	while ((ch = getopt(argc, argv, "pt:w")) != -1) {
		switch (ch) {
		case 'p':
			selection = XA_PRIMARY;
			break;
		case 't':
			targetstr = optarg;
			break;
		case 'w':
			wflag = 1;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	if (argc == 1 && (argv[0][0] != '-' || argv[0][1] != '\0') &&
	    (fd = open(argv[0], O_RDONLY)) == -1)
		err(EXIT_FAILURE, "%s", argv[0]);
	if (fstat(fd, &sb) == -1)
		err(EXIT_FAILURE, "fstat");
	size = sb.st_size;
	if ((data = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		mapped = 0;
		if ((data = malloc(size)) == NULL) {
			warn("malloc");
			goto error;
		}
		if (fillbuffer(fd, data, &size) == -1) {
			goto error;
		}
	}
	retval = xclipin(selection, targetstr, data, size, wflag);
error:
	if (mapped)
		munmap(data, sb.st_size);
	else
		free(data);
	(void)close(fd);
	return retval;
}
