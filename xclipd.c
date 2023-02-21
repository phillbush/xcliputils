#include <err.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include "ctrlsel.h"

enum Atom {
	ATOM_PAIR,
	CLIPBOARD,
	CLIPBOARD_MANAGER,
	DELETE,
	INCR,
	MANAGER,
	MULTIPLE,
	_NULL,
	PRIMARY,
	SAVE_TARGETS,
	TARGETS,
	TEXT,
	TIMESTAMP,
	UTF8_STRING,
	_TIMESTAMP_PROP,
	ATOM_LAST
};

enum Event {
	EV_NEWOWNER,    /* a new owner acquired the clipboard */
	EV_CLOSE,       /* we should close the manager */
	EV_OTHER,       /* another event (maybe related to a selection context) */
};

struct Manager {
	Time time;                      /* timestamp to own clipboard */
	Window window;                  /* manager window */
	int dowait;                     /* whether to wait for next owner */
	unsigned long ntargets;         /* number of targets the last owner supported */
	struct CtrlSelTarget *targets;  /* the targets last owner supported */
	struct CtrlSelContext *context; /* selection context for CLIPBOARD_MANAGER */
};

static Display *display = NULL;
static Atom atoms[ATOM_LAST] = { 0 };
static int xfixes = 0;
static int (*xerrorxlib)(Display *, XErrorEvent *) = NULL;
static char *atomnames[ATOM_LAST] = {
	[ATOM_PAIR]             = "ATOM_PAIR",
	[CLIPBOARD]             = "CLIPBOARD",
	[CLIPBOARD_MANAGER]     = "CLIPBOARD_MANAGER",
	[DELETE]                = "DELETE",
	[INCR]                  = "INCR",
	[MANAGER]               = "MANAGER",
	[MULTIPLE]              = "MULTIPLE",
	[_NULL]                 = "NULL",
	[PRIMARY]               = "PRIMARY",
	[SAVE_TARGETS]          = "SAVE_TARGETS",
	[TARGETS]               = "TARGETS",
	[TEXT]                  = "TEXT",
	[TIMESTAMP]             = "TIMESTAMP",
	[UTF8_STRING]           = "UTF8_STRING",
	[_TIMESTAMP_PROP]       = "_TIMESTAMP_PROP",
};

static int
xerror(Display *display, XErrorEvent *e)
{
	if (e->error_code == BadWindow)
		return 0;
	return xerrorxlib(display, e);
	exit(EXIT_FAILURE);             /* unreachable */
}

static Window
createwindow(Display *display)
{
	return XCreateWindow(
		display,
		DefaultRootWindow(display),
		0, 0, 1, 1, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWEventMask,
		&(XSetWindowAttributes){
			.event_mask = StructureNotifyMask | PropertyChangeMask,
		}
	);
}

static void
freetargets(struct Manager *manager)
{
	unsigned long i;

	for (i = 0; i < manager->ntargets; i++)
		free(manager->targets[i].buffer);
	free(manager->targets);
	manager->targets = NULL;
	manager->ntargets = 0;
}

static void
close(struct Manager *manager, int running)
{
	if (running) {
		freetargets(manager);
		ctrlsel_disown(manager->context);
	}
	if (manager->window != None)
		(void)XDestroyWindow(display, manager->window);
	if (display != NULL)
		(void)XCloseDisplay(display);
	exit(EXIT_FAILURE);
}

static enum Event
nextevent(struct Manager *manager, XEvent *xev)
{
	XFixesSelectionNotifyEvent *xselev;

loop:
	(void)XNextEvent(display, xev);
	xselev = ((XFixesSelectionNotifyEvent *)xev);
	switch (ctrlsel_send(manager->context, xev)) {
	case CTRLSEL_INTERNAL:
		/*
		 * Probably unreachable (the clipboard
		 * manager should not maintain internal
		 * state).
		 */
		goto loop;
	case CTRLSEL_ERROR:
		/*
		 * Probably unreachable (the clipboard
		 * manager should not maintain internal
		 * state).
		 */
		warnx("could not convert the manager selection");
		return EV_CLOSE;
	case CTRLSEL_LOST:
		/*
		 * We lost the manager selection to
		 * another clipboard manager.  Stop
		 * running.
		 */
		return EV_CLOSE;
	default:
		break;
	}
	if (xev->type == xfixes + XFixesSelectionNotify &&
	    xselev->selection == atoms[CLIPBOARD] &&
	    xselev->owner != manager->window) {
		/*
		 * Another client got the clipboard.
		 * Request its content.
		 */
		manager->time = xselev->timestamp;
		manager->dowait = 0;
		return EV_NEWOWNER;
	}
	if (xev->type == DestroyNotify &&
	    xev->xdestroywindow.window == manager->window) {
		/*
		 * Someone destroyed our window.
		 * Stop running.
		 */
		return EV_CLOSE;
	}
	return EV_OTHER;
}

static void
waitowner(struct Manager *manager)
{
	XEvent xev;

	freetargets(manager);
	for (;;) {
		switch(nextevent(manager, &xev)) {
		case EV_NEWOWNER:
			return;
		case EV_CLOSE:
			close(manager, 1);
			exit(EXIT_FAILURE);     /* unreachable */
		default:
			break;
		}
	}
	/* unreachable */
}

static int
gettargets(struct Manager *manager)
{
	XEvent xev;
	struct CtrlSelContext context;
	struct CtrlSelTarget meta;
	unsigned long i;
	Atom target;
	int success;

	freetargets(manager);
	ctrlsel_filltarget(atoms[TARGETS], XA_ATOM, 32, NULL, 0, &meta);
	success = ctrlsel_request(
		display,
		manager->window,
		atoms[CLIPBOARD],
		manager->time,
		&meta, 1,
		&context
	);
	if (!success)
		return 0;
	for (;;) {
		switch(nextevent(manager, &xev)) {
		case EV_NEWOWNER:
			ctrlsel_cancel(&context);
			freetargets(manager);
			return 0;
		case EV_CLOSE:
			ctrlsel_cancel(&context);
			close(manager, 1);
			exit(EXIT_FAILURE);     /* unreachable */
		default:
			break;
		}
		switch(ctrlsel_receive(&context, &xev)) {
		case CTRLSEL_RECEIVED:
			goto done;
		case CTRLSEL_ERROR:
			ctrlsel_cancel(&context);
			return 0;
		default:
			break;
		}
	}
done:
	ctrlsel_cancel(&context);
	if (meta.buffer == NULL || meta.nitems == 0)
		goto error;
	if (meta.format != 32 || meta.type != XA_ATOM)
		goto error;
	manager->targets = calloc(meta.nitems, sizeof(*manager->targets));
	if (manager->targets == NULL) {
		warn("calloc");
		goto error;
	}
	manager->ntargets = 0;
	for (i = 0; i < meta.nitems; i++) {
		target = ((Atom *)meta.buffer)[i];
		if (target == atoms[ATOM_PAIR] ||
		    target == atoms[CLIPBOARD] ||
		    target == atoms[CLIPBOARD_MANAGER] ||
		    target == atoms[DELETE] ||
		    target == atoms[INCR] ||
		    target == atoms[MANAGER] ||
		    target == atoms[MULTIPLE] ||
		    target == atoms[_NULL] ||
		    target == atoms[PRIMARY] ||
		    target == atoms[SAVE_TARGETS] ||
		    target == atoms[TARGETS] ||
		    target == atoms[TIMESTAMP] ||
		    target == atoms[_TIMESTAMP_PROP]) {
			continue;
		}
		ctrlsel_filltarget(
			((Atom *)meta.buffer)[i],
			((Atom *)meta.buffer)[i],
			8, NULL, 0,
			&manager->targets[manager->ntargets]
		);
		manager->ntargets++;
	}
	free(meta.buffer);
	return 1;
error:
	free(meta.buffer);
	return 0;
}

static int
savetargets(struct Manager *manager)
{
	XEvent xev;
	struct CtrlSelContext context;
	int success;

	success = ctrlsel_request(
		display,
		manager->window,
		atoms[CLIPBOARD],
		manager->time,
		manager->targets,
		manager->ntargets,
		&context
	);
	if (!success)
		return 0;
	for (;;) {
		switch(nextevent(manager, &xev)) {
		case EV_NEWOWNER:
			ctrlsel_cancel(&context);
			freetargets(manager);
			return 0;
		case EV_CLOSE:
			ctrlsel_cancel(&context);
			close(manager, 1);
			exit(EXIT_FAILURE);     /* unreachable */
		default:
			break;
		}
		switch(ctrlsel_receive(&context, &xev)) {
		case CTRLSEL_RECEIVED:
			ctrlsel_cancel(&context);
			return 1;
		case CTRLSEL_ERROR:
			ctrlsel_cancel(&context);
			return 0;
		default:
			break;
		}
	}
	/* unreachable */
}

static void
ownclipboard(struct Manager *manager)
{
	XEvent xev;
	struct CtrlSelContext clipboard, primary;

	ctrlsel_setowner(
		display,
		manager->window,
		atoms[CLIPBOARD],
		manager->time,
		0,
		manager->targets,
		manager->ntargets,
		&clipboard
	);
	ctrlsel_setowner(
		display,
		manager->window,
		atoms[PRIMARY],
		manager->time,
		0,
		manager->targets,
		manager->ntargets,
		&primary
	);
	for (;;) {
		switch(nextevent(manager, &xev)) {
		case EV_NEWOWNER:
			ctrlsel_disown(&clipboard);
			ctrlsel_disown(&primary);
			freetargets(manager);
			return;
		case EV_CLOSE:
			ctrlsel_disown(&clipboard);
			ctrlsel_disown(&primary);
			close(manager, 1);
			exit(EXIT_FAILURE);     /* unreachable */
		default:
			break;
		}
		(void)ctrlsel_send(&clipboard, &xev);
		(void)ctrlsel_send(&primary, &xev);
	}
	/* unreachable */
}

static void
init(struct Manager *manager)
{
	int i;

	if ((display = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		close(manager, 0);
	}
	if (!XFixesQueryExtension(display, &xfixes, &i)) {
		warnx("could not use XFixes");
		close(manager, 0);
	}
	if (!XInternAtoms(display, atomnames, ATOM_LAST, False, atoms)) {
		warnx("could not intern atoms");
		close(manager, 0);
	}
	if ((manager->window = createwindow(display)) == None) {
		warnx("could not create manager window");
		close(manager, 0);
	}
	if (XGetSelectionOwner(display, atoms[CLIPBOARD_MANAGER]) != None) {
		warnx("there's already a clipboard manager running");
		close(manager, 0);
	}
	if (!ctrlsel_setowner(display, manager->window, atoms[CLIPBOARD_MANAGER],
	                        CurrentTime, 1, NULL, 0, manager->context)) {
		warnx("could not own manager selection");
		close(manager, 0);
	}
	XFixesSelectSelectionInput(
		display,
		manager->window,
		atoms[CLIPBOARD],
		XFixesSetSelectionOwnerNotifyMask
	);
	xerrorxlib = XSetErrorHandler(xerror);
}

int
main(void)
{
	struct CtrlSelContext contex;
	struct Manager manager;

	manager = (struct Manager){
		.time = CurrentTime,
		.window = None,
		.dowait = 0,
		.ntargets = 0,
		.targets = NULL,
		.context = &contex,
	};
	init(&manager);
	for (;;) {
		if (manager.dowait)
			waitowner(&manager);
		manager.dowait = 1;
		if (!gettargets(&manager))
			continue;
		if (!savetargets(&manager))
			continue;
		ownclipboard(&manager);
	}
	/* unreachable */
	return EXIT_FAILURE;
}
