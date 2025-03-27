#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include <control/selection.h>

#include "util.h"

int
main(void)
{
	Display *display;
	Atom selection, manager_atm, targets_atm;
	Window watcher, manager;
	XEvent event;
	XFixesSelectionNotifyEvent *xselection = (void *)&event;
	static int xselection_event;

	display = xinit();
	selection = getatom(display, SELECTION);
	manager_atm = getatom(display, "CLIPBOARD_MANAGER");
	targets_atm = getatom(display, "TARGETS");
	watcher = createwindow(display);
	if (!XFixesQueryExtension(display, &xselection_event, (int[]){0}))
		errx(EXIT_FAILURE, "could not use XFixes");
	xselection_event += XFixesSelectionNotify;
	XFixesSelectSelectionInput(
		display, watcher, selection,
		XFixesSetSelectionOwnerNotifyMask
	);
	XFixesSelectSelectionInput(
		display, watcher, manager_atm,
		XFixesSetSelectionOwnerNotifyMask
	);
	XSync(display, False);
	manager = XGetSelectionOwner(display, manager_atm);

	while (!XNextEvent(display, &event)) if (event.type == DestroyNotify) {
		if (event.xdestroywindow.window == watcher)
			continue;
		errx(EXIT_FAILURE, "watcher window destroyed");
	} else if (event.type != xselection_event) {
		continue;
	} else if (xselection->selection == manager_atm) {
		manager = xselection->owner;
	} else if (xselection->selection == selection && xselection->owner != manager) {
		struct ctrlsel content = { .data = NULL };
		int status;
		Atom *targets = NULL;
		size_t ntargets = 0;

		status = ctrlsel_request(
			display, xselection->timestamp, selection,
			targets_atm, &content
		);
		if (status > 0 && content.format == 32 && content.type == XA_ATOM) {
			targets = content.data;
			ntargets = content.length;
		}

		printf("%010lu", xselection->selection_timestamp);
		printf("\t0x%08lX", xselection->owner);
		for (size_t i = 0; i < ntargets; i++) {
			char *name;

			name = XGetAtomName(display, targets[i]);
			printf("\t%s", name);
			XFree(name);
		}
		printf("\n");
		XFree(content.data);
	}
}
