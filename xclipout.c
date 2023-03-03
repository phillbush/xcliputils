#include <err.h>
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
	fprintf(stderr, "usage: xclipout [-p] [-t target]\n");
	exit(EXIT_FAILURE);
}

static int
datawrite(Display *display, struct CtrlSelTarget *target)
{
	size_t i, off;
	ssize_t nw;
	char *str;

	if (target->type == XA_ATOM) {
		for (i = 0; i < target->nitems; i++) {
			str = XGetAtomName(display, ((Atom *)target->buffer)[i]);
			printf("%s\n", str);
			XFree(str);
		}
	} else if (target->type == XA_CARDINAL) {
		for (i = 0; i < target->nitems; i++) {
			printf("%lu\n", ((unsigned long *)target->buffer)[i]);
		}
	} else if (target->type == XA_WINDOW) {
		for (i = 0; i < target->nitems; i++) {
			printf("0x%08lX\n", ((Window *)target->buffer)[i]);
		}
	} else {
		for (off = 0; off < target->bufsize; off += nw) {
			if ((nw = write(STDOUT_FILENO, target->buffer + off, target->bufsize - off)) == 0 || nw == -1) {
				return -1;
			}
		}
	}
	return 0;
}

static int
xclipout(Atom selection, char *targetstr)
{
	struct CtrlSelContext context;
	struct CtrlSelTarget target;
	XEvent xev;
	Display *display = NULL;
	Window window = None;
	Atom targetatom = None;
	int retval = EXIT_FAILURE;
	int success, status;

	if (!xinit(&display, &window))
		goto error;
#if __OpenBSD__
	if (pledge("stdio", NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
	if (selection == None && (selection = XInternAtom(display, CLIPBOARD, False)) == None) {
		warnx("could not intern atom");
		goto error;
	}
	if ((targetatom = XInternAtom(display, targetstr, False)) == None) {
		warnx("could not intern atom");
		goto error;
	}
	ctrlsel_filltarget(targetatom, targetatom, 8, NULL, 0, &target);
	success = ctrlsel_request(
		display,
		window,
		selection,
		CurrentTime,
		&target,
		1,
		&context
	);
	if (!success)
		goto error;
	for (;;) {
		(void)XNextEvent(display, &xev);
		status = ctrlsel_receive(&context, &xev);
		if (status == CTRLSEL_RECEIVED || status == CTRLSEL_ERROR) {
			break;
		}
	}
	ctrlsel_cancel(&context);
	if (status == CTRLSEL_ERROR)
		goto done;
	if (datawrite(display, &target) == -1)
		goto done;
	retval = EXIT_SUCCESS;
done:
	free(target.buffer);
error:
	xclose(display, window);
	return retval;
}

int
main(int argc, char *argv[])
{
	Atom selection = None;
	int ch;
	char *targetstr = "UTF8_STRING";

#if __OpenBSD__
	if (pledge("unix stdio rpath", NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
	while ((ch = getopt(argc, argv, "pt:")) != -1) {
		switch (ch) {
		case 'p':
			selection = XA_PRIMARY;
			break;
		case 't':
			targetstr = optarg;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 0)
		usage();
	return xclipout(selection, targetstr);
}
