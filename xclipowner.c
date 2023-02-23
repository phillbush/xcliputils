#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include "util.h"
#include "ctrlsel.h"

#define FORMAT          "0x%08lX\n"
#define CLIPBOARD       "CLIPBOARD"

static void
usage(void)
{
	fprintf(stderr, "usage: xclipowner [-pw]\n");
	exit(EXIT_FAILURE);
}

void
printowner(Display *display, Window window, int xfixes, Atom selection)
{
	Atom owner;

	(void)xfixes;
	(void)window;
	owner = XGetSelectionOwner(display, selection);
	printf(FORMAT, owner);
}

void
watchowner(Display *display, Window window, int xfixes, Atom selection)
{
	XEvent xev;
	XFixesSelectionNotifyEvent *xselev;

	printowner(display, window, xfixes, selection);
	for (;;) {
		(void)XNextEvent(display, &xev);
		xselev = ((XFixesSelectionNotifyEvent *)&xev);
		if (xev.type == DestroyNotify &&
		    xev.xdestroywindow.window == window)
			return;
		if (xev.type == xfixes + XFixesSelectionNotify &&
		    xselev->selection == selection) {
			printf(FORMAT, xselev->owner);
			fflush(stdout);
		}
	}
}

int
main(int argc, char *argv[])
{
	void (*func)(Display *display, Window window, int xfixes, Atom selection);
	Display *display = NULL;
	Window window = None;
	Atom selection = None;
	int retval = EXIT_FAILURE;
	int xfixes, ch, i;

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
	if (!XFixesQueryExtension(display, &xfixes, &i)) {
		warnx("could not use XFixes");
		goto error;
	}
	if (selection == None && (selection = XInternAtom(display, CLIPBOARD, False)) == None) {
		warnx("could not intern atom");
		goto error;
	}
	XFixesSelectSelectionInput(
		display,
		window,
		selection,
		XFixesSetSelectionOwnerNotifyMask
	);
	(*func)(display, window, xfixes, selection);
	retval = EXIT_SUCCESS;
error:
	xclose(display, window);
	return retval;
}
