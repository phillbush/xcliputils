#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include "util.h"

static void
usage(void)
{
	(void)fprintf(stderr, "usage: xclipowner [-pw]\n");
	exit(EXIT_FAILURE);
}

void
printwinid(Window win)
{
	(void)printf("0x%08lX\n", win);
	fflush(stdout);
}

int
printowner(Display *display, Window window, int xfixes, Atom selection)
{
	(void)xfixes;
	(void)window;
	printwinid(XGetSelectionOwner(display, selection));
	return 1;
}

int
watchowner(Display *display, Window window, int xfixes, Atom selection)
{
	XEvent xev;
	XFixesSelectionNotifyEvent *xselev;
	Atom manageratom;
	Window manager;

	manageratom = XInternAtom(display, "CLIPBOARD_MANAGER", False);
	if (manageratom == None) {
		warnx("could not intern atom");
		return 0;
	}
	XFixesSelectSelectionInput(
		display,
		window,
		manageratom,
		XFixesSetSelectionOwnerNotifyMask
	);
	manager = XGetSelectionOwner(display, manageratom);
	printowner(display, window, xfixes, selection);
	while (!XNextEvent(display, &xev)) switch (xev.type) {
	case DestroyNotify:
		if (xev.xdestroywindow.window != window)
			break;
		return 1;
	default:
		if (xev.type != xfixes + XFixesSelectionNotify)
			break;
		xselev = (XFixesSelectionNotifyEvent *)&xev;
		if (xselev->selection == manageratom) {
			manager = xselev->owner;
			break;
		}
		if (xselev->selection != selection)
			break;
		if (xselev->owner != manager)
			printwinid(xselev->owner);
		break;
	}
	/* unreachable */
	return 0;
}

int
main(int argc, char *argv[])
{
	int (*func)(Display *, Window, int, Atom);
	Display *display = NULL;
	Window window = None;
	Atom selection = None;
	int exitval = EXIT_FAILURE;
	int xfixes, ch, i;

#if __OpenBSD__
	if (pledge("unix stdio rpath", NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
	func = &printowner;
	while ((ch = getopt(argc, argv, "pw")) != -1) {
		switch (ch) {
		case 'p':
			selection = XA_PRIMARY;
			break;
		case 'w':
			func = &watchowner;
			break;
		default:
			usage();
			break;
		}
	}
	if (!xinit(&display, &window))
		goto error;
#if __OpenBSD__
	if (pledge("stdio", NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
	if (!XFixesQueryExtension(display, &xfixes, &i)) {
		warnx("could not use XFixes");
		goto error;
	}
	if (selection == None)
		selection = XInternAtom(display, "CLIPBOARD", False);
	if (selection == None) {
		warnx("could not intern atom");
		goto error;
	}
	XFixesSelectSelectionInput(
		display,
		window,
		selection,
		XFixesSetSelectionOwnerNotifyMask
	);
	if (!(*func)(display, window, xfixes, selection))
		goto error;
	exitval = EXIT_SUCCESS;
error:
	xclose(display, window);
	return exitval;
}
