#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include <control/selection.h>

#include "util.h"

#define ENUM(sym, str) sym,
#define NAME(sym, str) (str==NULL?#sym:str),
#define ATOMS(X) \
	X(CLIPBOARD,		NULL) \
	X(CLIPBOARD_MANAGER,	NULL) \
	X(DELETE,		NULL) \
	X(MULTIPLE,		NULL) \
	X(SAVE_TARGETS,		NULL) \
	X(TARGETS,		NULL) \
	X(TEXT,			NULL) \
	X(TIMESTAMP,		NULL) \
	X(INSERT_PROPERTY,	NULL) \
	X(INSERT_SELECTION,	NULL) \
	X(UTF8_STRING,		NULL) \
	X(STRING,		NULL) \
	X(TEXT_PLAIN,		"text/plain") \
	X(TEXT_PLAIN_UTF8,	"text/plain;charset=utf-8") \

enum atoms {
	ATOMS(ENUM)
	NATOMS
};

struct clipboard {
	struct ctrlsel *contents;
	Atom *targets;
	size_t ntargets;
};

static Display *display;
static Window manager;
static Atom atomtab[NATOMS];
static int xselection_event;

static int
callback(void *arg, Atom target, struct ctrlsel *content)
{
	struct clipboard *clip = arg;

	for (size_t i = 0; i < clip->ntargets; i++) {
		if (target == clip->targets[i]) {
			*content = clip->contents[i];
			return True;
		}
	}
	return False;
}

static Time
next_clipboard(Time epoch, struct clipboard *clip)
{
	XEvent event;
	XFixesSelectionNotifyEvent *xselection = (void *)&event;
	int error;

	for (;;) switch (XNextEvent(display, &event), event.type) {
	case SelectionRequest:
		if (clip == NULL || clip->ntargets == 0)
			continue;
		if (event.xselectionrequest.owner != manager)
			continue;
		if (event.xselectionrequest.selection != atomtab[CLIPBOARD] &&
		    event.xselectionrequest.selection != XA_PRIMARY)
			continue;
		error = -ctrlsel_answer(
			&event, epoch,
			clip->targets, clip->ntargets,
			callback, clip
		);
		if (error) warnx(
			"could not answer client 0x%08lX: %s",
			event.xselectionrequest.requestor,
			strerror(error)
		);
		continue;
	case SelectionClear:
		if (event.xselectionclear.window != manager)
			continue;
		if (event.xselectionclear.selection == atomtab[CLIPBOARD_MANAGER])
			return 0;
		continue;
	case DestroyNotify:
		if (event.xdestroywindow.window == manager)
			return 0;
		continue;
	default:
		if (event.type != xselection_event)
			continue;
		if (xselection->selection != atomtab[CLIPBOARD])
			continue;
		if (xselection->owner == manager || xselection->owner == None)
			continue;
		return xselection->timestamp;
	}
	return 0;
}

static size_t
gettargets(Time timestamp, Atom **targets)
{
	struct ctrlsel content = { 0 };

	if (ctrlsel_request(
		display, timestamp, atomtab[CLIPBOARD],
		atomtab[TARGETS], &content
	) > 0 && content.format == 32 && content.type == XA_ATOM) {
		*targets = content.data;
		return content.length;
	}
	XFree(content.data);
	*targets = NULL;
	return 0;
}

int
main(void)
{
	char *atomnames[] = { ATOMS(NAME) };
	Time timestamp;
	struct ctrlsel *buf = NULL;

	display = xinit();
	manager = createwindow(display);
	if (!XInternAtoms(display, atomnames, NATOMS, False, atomtab))
		errx(EXIT_FAILURE, "could not intern atoms");
	if (XGetSelectionOwner(display, atomtab[CLIPBOARD_MANAGER]) != None)
		errx(EXIT_FAILURE, "there's already another clipboard manager running");
	timestamp = ctrlsel_own(display, manager, CurrentTime, atomtab[CLIPBOARD_MANAGER]);
	if (timestamp == 0)
		errx(EXIT_FAILURE, "could not own clipboard manager");
	if (!XFixesQueryExtension(display, &xselection_event, (int[]){0}))
		errx(EXIT_FAILURE, "could not use XFixes");
	xselection_event += XFixesSelectionNotify;
	XFixesSelectSelectionInput(
		display, manager, atomtab[CLIPBOARD],
		XFixesSetSelectionOwnerNotifyMask
	);

	do {
		size_t n;
		Time epoch;
		struct clipboard clip;

		n = gettargets(timestamp, &clip.targets);
		for (size_t i = clip.ntargets = 0; i < n; i++) {
			/* discard meta-targets */
			if (clip.targets[i] == atomtab[TARGETS] ||
			    clip.targets[i] == atomtab[MULTIPLE] ||
			    clip.targets[i] == atomtab[TIMESTAMP] ||
			    clip.targets[i] == atomtab[DELETE] ||
			    clip.targets[i] == atomtab[INSERT_PROPERTY] ||
			    clip.targets[i] == atomtab[INSERT_SELECTION])
				continue;
			clip.targets[clip.ntargets++] = clip.targets[i];
		}

		clip.contents = NULL;
		if (clip.ntargets > 0) {
			clip.contents = reallocarray(buf, clip.ntargets, sizeof(*buf));
		}
		if (clip.contents == NULL) {
			XFree(clip.targets);
			timestamp = next_clipboard(0, NULL);
			continue;
		}
		buf = clip.contents;

		for (size_t i = 0; i < clip.ntargets; i++) {
			clip.contents[i].data = NULL;
			(void)ctrlsel_request(
				display, timestamp, atomtab[CLIPBOARD],
				clip.targets[i], &clip.contents[i]
			);
		}

		epoch = ctrlsel_own(
			display, manager, timestamp, atomtab[CLIPBOARD]
		);
		(void)ctrlsel_own(
			display, manager, timestamp, XA_PRIMARY
		);

		timestamp = next_clipboard(epoch, &clip);
		XFree(clip.targets);
		for (size_t i = 0; i < clip.ntargets; i++) {
			XFree(clip.contents[i].data);
		}
	} while (timestamp != 0);
	free(buf);
	XDestroyWindow(display, manager);
	XCloseDisplay(display);
	return EXIT_FAILURE;
}
