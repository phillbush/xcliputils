#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <control/selection.h>

#include "util.h"

int
main(int argc, char *argv[])
{
	struct ctrlsel content;
	Display *display;
	ssize_t ntargets;
	Atom target = None;
	Atom selection;
	Atom *targets;
	Time timestamp;
	char **requests = (char *[]){ "UTF8_STRING", "STRING", "TEXT", NULL };

	if (argc > 1)
		requests = argv + 1;
	display = xinit();
	selection = getatom(display, SELECTION);
	timestamp = getservertime(display);
	if (timestamp == 0)
		errx(EXIT_FAILURE, "cannot get server time");
	if (ctrlsel_request(
		display, timestamp, selection,
		getatom(display, "TARGETS"), &content
	) <= 0 || content.format != 32 || content.type != XA_ATOM) {
		XFree(content.data);
		return EXIT_FAILURE;
	}
	targets = content.data;
	ntargets = content.length;
	for (int i = 0; requests[i] != NULL; i++) {
		Atom atom;

		atom = XInternAtom(display, requests[i], True);
		if (atom == None)
			continue;
		for (ssize_t i = 0; i < ntargets; i++) {
			if (atom == targets[i]) {
				target = atom;
				break;
			}
		}
		if (target != None)
			break;
	}
	XFree(targets);
	if (target == None)
		errx(EXIT_FAILURE, "cannot convert selection to any requested target");
	if (ctrlsel_request(
		display, timestamp, selection, target, &content
	) < 0 || content.format != 32 || content.type != XA_INTEGER) {
		errx(EXIT_FAILURE, "cannot convert selection");
	}
	if (fwrite(content.data, 1, content.length, stdout) != (size_t)content.length)
		warn(NULL);
	XFree(content.data);
	XCloseDisplay(display);
	return EXIT_SUCCESS;
}
