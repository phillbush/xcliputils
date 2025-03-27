#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <control/selection.h>

#include "util.h"

int
main(void)
{
	Display *display;
	struct ctrlsel content = { .data = NULL };
	Atom selection;
	Atom *targets;
	Time timestamp, epoch;
	size_t ntargets;
	Window owner;
	int status;

	display = xinit();
	selection = getatom(display, SELECTION);
	timestamp = getservertime(display);
	if (timestamp == 0)
		errx(EXIT_FAILURE, "cannot get server time");

	/* 2nd field: owner */
	/* must be get before timestamp to circumvent race conditions */
	owner = XGetSelectionOwner(display, selection);
	if (owner == None)
		return EXIT_FAILURE;

	/* 1st field: epoch */
	status = ctrlsel_request(
		display, timestamp, selection,
		getatom(display, "TIMESTAMP"), &content
	);
	if (status <= 0 || content.format != 32 || content.length != 1 ||
	    (content.type != XA_INTEGER && content.type != XA_CARDINAL)) {
		errx(
			EXIT_FAILURE, "ill selection owner: %s",
			"cannot get selection ownership time"
		);
	}
	epoch = *(long *)content.data;
	XFree(content.data);
	if (timestamp < epoch) /* selection ownership changed (race condition) */
		return EXIT_FAILURE;

	/* 3rd field: supported targets */
	status = ctrlsel_request(
		display, timestamp, selection,
		getatom(display, "TARGETS"), &content
	);
	if (status <= 0 || content.format != 32 || content.type != XA_ATOM) {
		errx(
			EXIT_FAILURE, "ill selection owner: %s",
			"cannot get list of supported targets"
		);
	}
	targets = content.data;
	ntargets = content.length;

	printf("%010lu", epoch);
	printf("\t0x%08lX", owner);
	for (size_t i = 0; i < ntargets; i++) {
		char *name;

		name = XGetAtomName(display, targets[i]);
		printf("\t%s", name);
		XFree(name);
	}
	printf("\n");

	XFree(content.data);
	XCloseDisplay(display);
	return EXIT_SUCCESS;
}
