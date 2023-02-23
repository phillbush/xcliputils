#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "ctrlsel.h"

#define _TIMESTAMP_PROP "_TIMESTAMP_PROP"
#define TIMESTAMP       "TIMESTAMP"
#define ATOM_PAIR       "ATOM_PAIR"
#define MULTIPLE        "MULTIPLE"
#define MANAGER         "MANAGER"
#define TARGETS         "TARGETS"
#define INCR            "INCR"
#define SELDEFSIZE      0x4000

enum {
	CONTENT_INCR,
	CONTENT_ZERO,
	CONTENT_ERROR,
	CONTENT_SUCCESS,
};

enum {
	PAIR_TARGET,
	PAIR_PROPERTY,
	PAIR_LAST
};

struct Transfer {
	/*
 	 * When a client request the clipboard but its content is too
 	 * large, we perform incremental transfer.  We keep track of
 	 * each incremental transfer in a list of transfers.
 	 */
	struct Transfer *prev, *next;
	struct CtrlSelTarget *target;
	Window requestor;
	Atom property;
	unsigned long size;     /* how much have we transferred */
};

static unsigned long
getselmaxsize(Display *display)
{
	unsigned long n;

	if ((n = XExtendedMaxRequestSize(display)) > 0)
		return n;
	if ((n = XMaxRequestSize(display)) > 0)
		return n;
	return SELDEFSIZE;
}

static Time
getservertime(Display *display, Window window)
{
	XEvent xev;
	Atom timeprop;

	/*
	 * According to ICCCM, a client wishing to acquire ownership of
	 * a selection should set the specfied time to some time between
	 * the current last-change time of the selection concerned and
	 * the current server time.
	 *
	 * Those clients should not set the time value to `CurrentTime`,
	 * because if they do so, they have no way of finding when they
	 * gained ownership of the selection.
	 *
	 * In the case that an event triggers the acquisition of the
	 * selection, this time value can be obtained from the event
	 * itself.
	 *
	 * In the case that the client must inconditionally acquire the
	 * ownership of a selection (which is our case), a zero-length
	 * append to a property is a way to obtain a timestamp for this
	 * purpose.  The timestamp is in the corresponding
	 * `PropertyNotify` event.
	 *
	 * Note that, for this second method, the window used to get
	 * the `PropertyNotify` event from the server (which, in our
	 * case, is the window that will own the selection) must have
	 * `PropertyChangeMask` in its event mask.  Otherwise, a hang
	 * will result, for the window could not receive such event.
	 */

	timeprop = XInternAtom(display, _TIMESTAMP_PROP, False),
	XChangeProperty(
		display, window,
		timeprop, timeprop,
		8L, PropModeAppend, NULL, 0
	);
	while (!XNextEvent(display, &xev)) {
		if (xev.type == PropertyNotify &&
		    xev.xproperty.window == window &&
		    xev.xproperty.atom == timeprop) {
			return xev.xproperty.time;
		}
	}
	/* unreachable */
	return CurrentTime;
}

static int
nbytes(int format)
{
	switch (format) {
	default: return sizeof(char);
	case 16: return sizeof(short);
	case 32: return sizeof(long);
	}
}

static int
getcontent(struct CtrlSelTarget *target, Display *display, Window window, Atom property)
{
	unsigned char *p, *q;
	unsigned long len, addsize, size;
	unsigned long dl;   /* dummy variable */
	int status;
	Atom incr;

	incr = XInternAtom(display, INCR, False),
	status = XGetWindowProperty(
		display,
		window,
		property,
		0L, 0x1FFFFFFF,
		True,
		AnyPropertyType,
		&target->type,
		&target->format,
		&len, &dl, &p
	);
	if (target->format != 32 && target->format != 16)
		target->format = 8;
	if (target->type == incr) {
		XFree(p);
		return CONTENT_INCR;
	}
	if (len == 0) {
		XFree(p);
		return CONTENT_ZERO;
	}
	if (status != Success) {
		XFree(p);
		return CONTENT_ERROR;
	}
	if (p == NULL) {
		XFree(p);
		return CONTENT_ERROR;
	}
	addsize = len * nbytes(target->format);
	size = addsize;
	if (target->buffer != NULL) {
		/* append buffer */
		size += target->bufsize;
		if ((q = realloc(target->buffer, size)) == NULL) {
			XFree(p);
			return CONTENT_ERROR;
		}
		memcpy(q + target->bufsize, p, addsize);
		target->buffer = q;
		target->bufsize = size;
		target->nitems += len;
	} else {
		/* new buffer */
		if ((q = malloc(size)) == NULL) {
			XFree(p);
			return CONTENT_ERROR;
		}
		memcpy(q, p, addsize);
		target->buffer = q;
		target->bufsize = size;
		target->nitems = len;
	}
	XFree(p);
	return CONTENT_SUCCESS;
}

static void
deltransfer(struct CtrlSelContext *context, struct Transfer *transfer)
{
	if (transfer->prev != NULL) {
		transfer->prev->next = transfer->next;
	} else {
		context->transfers = transfer->next;
	}
	if (transfer->next != NULL) {
		transfer->next->prev = transfer->prev;
	}
}

static void
freetransferences(struct CtrlSelContext *context)
{
	struct Transfer *transfer;

	while (context->transfers != NULL) {
		transfer = (struct Transfer *)context->transfers;
		context->transfers = ((struct Transfer *)context->transfers)->next;
		XDeleteProperty(
			context->display,
			transfer->requestor,
			transfer->property
		);
		free(transfer);
	}
	context->transfers = NULL;
}

static void
freebuffers(struct CtrlSelContext *context)
{
	unsigned long i;

	for (i = 0; i < context->ntargets; i++) {
		free(context->targets[i].buffer);
		context->targets[i].buffer = NULL;
		context->targets[i].nitems = 0;
		context->targets[i].bufsize = 0;
	}
}

static unsigned long
getatomsprop(Display *display, Window window, Atom property, Atom type, Atom **atoms)
{
	unsigned char *p;
	unsigned long len;
	unsigned long dl;       /* dummy variable */
	int format;
	Atom gottype;
	unsigned long size;
	int success;

	success = XGetWindowProperty(
		display,
		window,
		property,
		0L, 0x1FFFFFFF,
		False,
		type, &gottype,
		&format, &len,
		&dl, &p
	);
	if (success != Success || len == 0 || p == NULL ||
	    gottype != type || format != 32)
		goto error;
	size = len * sizeof(**atoms);
	if ((*atoms = malloc(size)) == NULL)
		goto error;
	memcpy(*atoms, p, size);
	XFree(p);
	return len;
error:
	XFree(p);
	*atoms = NULL;
	return 0;
}

static int
newtransfer(struct CtrlSelContext *context, struct CtrlSelTarget *target, Window requestor, Atom property)
{
	struct Transfer *transfer;

	transfer = malloc(sizeof(*transfer));
	if (transfer == NULL)
		return 0;
	*transfer = (struct Transfer){
		.prev = NULL,
		.next = (struct Transfer *)context->transfers,
		.requestor = requestor,
		.property = property,
		.target = target,
		.size = 0,
	};
	if (context->transfers != NULL)
		((struct Transfer *)context->transfers)->prev = transfer;
	context->transfers = transfer;
	return 1;
}

static Bool
convert(struct CtrlSelContext *context, Window requestor, Atom target, Atom property)
{
	Atom multiple, timestamp, targets, incr;
	Atom *supported;
	unsigned long i;
	int nsupported;

	incr = XInternAtom(context->display, INCR, False);
	targets = XInternAtom(context->display, TARGETS, False);
	multiple = XInternAtom(context->display, MULTIPLE, False);
	timestamp = XInternAtom(context->display, TIMESTAMP, False);
	if (target == multiple) {
		/* A MULTIPLE should be handled when processing a
		 * SelectionRequest event.  We do not support nested
		 * MULTIPLE targets.
		 */
		return False;
	}
	if (target == timestamp) {
		/*
		 * According to ICCCM, to avoid some race conditions, it
		 * is important that requestors be able to discover the
		 * timestamp the owner used to acquire ownership.
		 * Requestors do that by requesting selection owners to
		 * convert the `TIMESTAMP` target.  Selection owners
		 * must return the timestamp as an `XA_INTEGER`.
		 */
		XChangeProperty(
			context->display,
			requestor,
			property,
			XA_INTEGER, 32,
			PropModeReplace,
			(unsigned char *)&context->time,
			1
		);
		return True;
	}
	if (target == targets) {
		/*
		 * According to ICCCM, when requested for the `TARGETS`
		 * target, the selection owner should return a list of
		 * atoms representing the targets for which an attempt
		 * to convert the selection will (hopefully) succeed.
		 */
		nsupported = context->ntargets + 2;     /* +2 for MULTIPLE + TIMESTAMP */
		if ((supported = calloc(nsupported, sizeof(*supported))) == NULL)
			return False;
		for (i = 0; i < context->ntargets; i++) {
			supported[i] = context->targets[i].target;
		}
		supported[i++] = multiple;
		supported[i++] = timestamp;
		XChangeProperty(
			context->display,
			requestor,
			property,
			XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)supported,
			nsupported
		);
		free(supported);
		return True;
	}
	for (i = 0; i < context->ntargets; i++) {
		if (target == context->targets[i].target)
			goto found;
	}
	return False;
found:
	if (context->targets[i].bufsize > context->selmaxsize) {
		XSelectInput(
			context->display,
			requestor,
			StructureNotifyMask | PropertyChangeMask
		);
		XChangeProperty(
			context->display,
			requestor,
			property,
			incr,
			32L,
			PropModeReplace,
			(unsigned char *)context->targets[i].buffer,
			1
		);
		newtransfer(context, &context->targets[i], requestor, property);
	} else {
		XChangeProperty(
			context->display,
			requestor,
			property,
			target,
			context->targets[i].format,
			PropModeReplace,
			context->targets[i].buffer,
			context->targets[i].nitems
		);
	}
	return True;
}

void
ctrlsel_filltarget(
	Atom target,
	Atom type,
	int format,
	unsigned char *buffer,
	unsigned long size,
	struct CtrlSelTarget *fill
) {
	if (fill == NULL)
		return;
	if (format != 32 && format != 16)
		format = 8;
	*fill = (struct CtrlSelTarget){
		.target = target,
		.type = type,
		.format = format,
		.nitems = size / nbytes(format),
		.buffer = buffer,
		.bufsize = size,
	};
}

int
ctrlsel_request(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	struct CtrlSelContext *context
) {
	Atom multiple, atom_pair;
	Atom *pairs;
	unsigned long i, size;

	if (time == CurrentTime)
		time = getservertime(display, window);
	*context = (struct CtrlSelContext){
		.display = display,
		.window = window,
		.selection = selection,
		.time = time,
		.targets = targets,
		.ntargets = ntargets,
		.selmaxsize = getselmaxsize(display),
		.ndone = 0,
		.transfers = NULL,
	};
	if (ntargets == 0)
		return 1;
	for (i = 0; i < ntargets; i++) {
		targets[i].nitems = 0;
		targets[i].bufsize = 0;
		targets[i].buffer = NULL;
	}
	if (ntargets == 1) {
		(void)XConvertSelection(
			display,
			selection,
			targets[0].target,
			targets[0].target,
			window,
			time
		);
	} else {
		multiple = XInternAtom(display, MULTIPLE, False);
		atom_pair = XInternAtom(display, ATOM_PAIR, False);
		size = 2 * ntargets;
		pairs = calloc(size, sizeof(*pairs));
		if (pairs == NULL)
			return 0;
		for (i = 0; i < ntargets; i++) {
			pairs[i * 2 + 0] = targets[i].target;
			pairs[i * 2 + 1] = targets[i].target;
		}
		(void)XChangeProperty(
			display,
			window,
			multiple,
			atom_pair,
			32,
			PropModeReplace,
			(unsigned char *)pairs,
			size
		);
		(void)XConvertSelection(
			display,
			selection,
			multiple,
			multiple,
			window,
			time
		);
		free(pairs);
	}
	return 1;
}

int
ctrlsel_setowner(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	int ismanager,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	struct CtrlSelContext *context
) {
	Window root;

	root = DefaultRootWindow(display);
	if (time == CurrentTime)
		time = getservertime(display, window);
	*context = (struct CtrlSelContext){
		.display = display,
		.window = window,
		.selection = selection,
		.time = time,
		.targets = targets,
		.ntargets = ntargets,
		.selmaxsize = getselmaxsize(display),
		.ndone = 0,
		.transfers = NULL,
	};
	(void)XSetSelectionOwner(display, selection, window, time);
	if (XGetSelectionOwner(display, selection) != window)
		return 0;
	if (!ismanager)
		return 1;

	/*
	 * According to ICCCM, a manager client (that is, a client
	 * responsible for managing shared resources) should take
	 * ownership of an appropriate selection.
	 *
	 * Immediately after a manager successfully acquires ownership
	 * of a manager selection, it should announce its arrival by
	 * sending a `ClientMessage` event.  (That is necessary for
	 * clients to be able to know when a specific manager has
	 * started: any client that wish to do so should select for
	 * `StructureNotify` on the root window and should watch for
	 * the appropriate `MANAGER` `ClientMessage`).
	 */
	(void)XSendEvent(
		display,
		root,
		False,
		StructureNotifyMask,
		(XEvent *)&(XClientMessageEvent){
			.type         = ClientMessage,
			.window       = root,
			.message_type = XInternAtom(display, MANAGER, False),
			.format       = 32,
			.data.l[0]    = time,           /* timestamp */
			.data.l[1]    = selection,      /* manager selection atom */
			.data.l[2]    = window,         /* window owning the selection */
			.data.l[3]    = 0,              /* manager-specific data */
			.data.l[4]    = 0,              /* manager-specific data */
		}
	);
	return 1;
}

static int
receiveinit(struct CtrlSelContext *context, XEvent *xev)
{
	struct CtrlSelTarget *targetp;
	XSelectionEvent *xselev;
	Atom multiple, atom_pair;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	unsigned long j, natoms;
	unsigned long i;
	int status, success;

	multiple = XInternAtom(context->display, MULTIPLE, False);
	atom_pair = XInternAtom(context->display, ATOM_PAIR, False);
	xselev = &xev->xselection;
	if (xselev->selection != context->selection)
		return CTRLSEL_NONE;
	if (xselev->requestor != context->window)
		return CTRLSEL_NONE;
	if (xselev->property == None)
		return CTRLSEL_ERROR;
	if (xselev->target == multiple) {
		natoms = getatomsprop(
			xselev->display,
			xselev->requestor,
			xselev->property,
			atom_pair,
			&pairs
		);
		if (natoms == 0 || pairs == NULL) {
			free(pairs);
			return CTRLSEL_ERROR;
		}
	} else {
		pair[PAIR_TARGET] = xselev->target;
		pair[PAIR_PROPERTY] = xselev->property;
		pairs = pair;
		natoms = 2;
	}
	success = 1;
	for (j = 0; j < natoms; j += 2) {
		targetp = NULL;
		for (i = 0; i < context->ntargets; i++) {
			if (pairs[j + PAIR_TARGET] == context->targets[i].target) {
				targetp = &context->targets[i];
				break;
			}
		}
		if (pairs[j + PAIR_PROPERTY] == None)
			pairs[j + PAIR_PROPERTY] = pairs[j + PAIR_TARGET];
		if (targetp == NULL) {
			success = 0;
			continue;
		}
		status = getcontent(
			targetp,
			xselev->display,
			xselev->requestor,
			pairs[j + PAIR_PROPERTY]
		);
		switch (status) {
		case CONTENT_ERROR:
			success = 0;
			break;
		case CONTENT_SUCCESS:
			/* fallthrough */
		case CONTENT_ZERO:
			context->ndone++;
			break;
		case CONTENT_INCR:
			if (!newtransfer(context, targetp, xselev->requestor, pairs[j + PAIR_PROPERTY]))
				success = 0;
			break;
		}
	}
	if (xselev->target == multiple)
		free(pairs);
	return success ? CTRLSEL_INTERNAL : CTRLSEL_ERROR;
}

static int
receiveincr(struct CtrlSelContext *context, XEvent *xev)
{
	struct Transfer *transfer;
	XPropertyEvent *xpropev;
	int status;

	xpropev = &xev->xproperty;
	if (xpropev->state != PropertyNewValue)
		return CTRLSEL_NONE;
	if (xpropev->window != context->window)
		return CTRLSEL_NONE;
	for (transfer = (struct Transfer *)context->transfers; transfer != NULL; transfer = transfer->next)
		if (transfer->property == xpropev->atom)
			goto found;
	return CTRLSEL_NONE;
found:
	status = getcontent(
		transfer->target,
		xpropev->display,
		xpropev->window,
		xpropev->atom
	);
	switch (status) {
	case CONTENT_ERROR:
	case CONTENT_INCR:
		return CTRLSEL_ERROR;
	case CONTENT_SUCCESS:
		return CTRLSEL_INTERNAL;
	case CONTENT_ZERO:
		context->ndone++;
		deltransfer(context, transfer);
		break;
	}
	return CTRLSEL_INTERNAL;
}

int
ctrlsel_receive(struct CtrlSelContext *context, XEvent *xev)
{
	int status;

	if (xev->type == SelectionNotify)
		status = receiveinit(context, xev);
	else if (xev->type == PropertyNotify)
		status = receiveincr(context, xev);
	else
		return CTRLSEL_NONE;
	if (status == CTRLSEL_INTERNAL) {
		if (context->ndone >= context->ntargets) {
			return CTRLSEL_RECEIVED;
		}
	} else if (status == CTRLSEL_ERROR) {
		freebuffers(context);
		freetransferences(context);
	}
	return status;
}

static int
sendinit(struct CtrlSelContext *context, XEvent *xev)
{
	XSelectionRequestEvent *xreqev;
	XSelectionEvent xselev;
	unsigned long natoms, i;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	Atom multiple, atom_pair;
	Bool success;

	xreqev = &xev->xselectionrequest;
	if (xreqev->selection != context->selection)
		return CTRLSEL_NONE;
	multiple = XInternAtom(context->display, MULTIPLE, False);
	atom_pair = XInternAtom(context->display, ATOM_PAIR, False);
	xselev = (XSelectionEvent){
		.type           = SelectionNotify,
		.display        = xreqev->display,
		.requestor      = xreqev->requestor,
		.selection      = xreqev->selection,
		.time           = xreqev->time,
		.target         = xreqev->target,
		.property       = None,
	};
	if (xreqev->time != CurrentTime && xreqev->time < context->time) {
		/*
		 * According to ICCCM, the selection owner
		 * should compare the timestamp with the period
		 * it has owned the selection and, if the time
		 * is outside, refuse the `SelectionRequest` by
		 * sending the requestor window a
		 * `SelectionNotify` event with the property set
		 * to `None` (by means of a `SendEvent` request
		 * with an empty event mask).
		 */
		goto done;
	}
	if (xreqev->target == multiple) {
		if (xreqev->property == None)
			goto done;
		natoms = getatomsprop(
			xreqev->display,
			xreqev->requestor,
			xreqev->property,
			atom_pair,
			&pairs
		);
	} else {
		pair[PAIR_TARGET] = xreqev->target;
		pair[PAIR_PROPERTY] = xreqev->property;
		pairs = pair;
		natoms = 2;
	}
	success = True;
	for (i = 0; i < natoms; i += 2) {
		if (!convert(context, xreqev->requestor,
		             pairs[i + PAIR_TARGET],
		             pairs[i + PAIR_PROPERTY])) {
			success = False;
			pairs[i + PAIR_PROPERTY] = None;
		}
	}
	if (xreqev->target == multiple) {
		XChangeProperty(
			xreqev->display,
			xreqev->requestor,
			xreqev->property,
			atom_pair,
			32, PropModeReplace,
			(unsigned char *)pairs,
			natoms
		);
		free(pairs);
	}
	if (success) {
		if (xreqev->property == None) {
			xselev.property = xreqev->target;
		} else {
			xselev.property = xreqev->property;
		}
	}
done:
	XSendEvent(
		xreqev->display,
		xreqev->requestor,
		False,
		NoEventMask,
		(XEvent *)&xselev
	);
	return CTRLSEL_INTERNAL;
}

static int
sendlost(struct CtrlSelContext *context, XEvent *xev)
{
	XSelectionClearEvent *xclearev;

	xclearev = &xev->xselectionclear;
	if (xclearev->selection == context->selection &&
	    xclearev->window == context->window) {
		return CTRLSEL_LOST;
	}
	return CTRLSEL_NONE;
}

static int
senddestroy(struct CtrlSelContext *context, XEvent *xev)
{
	struct Transfer *transfer;
	XDestroyWindowEvent *xdestroyev;

	xdestroyev = &xev->xdestroywindow;
	for (transfer = context->transfers; transfer != NULL; transfer = transfer->next)
		if (transfer->requestor == xdestroyev->window)
			deltransfer(context, transfer);
	return CTRLSEL_NONE;
}

static int
sendincr(struct CtrlSelContext *context, XEvent *xev)
{
	struct Transfer *transfer;
	XPropertyEvent *xpropev;
	unsigned long size;

	xpropev = &xev->xproperty;
	if (xpropev->state != PropertyDelete)
		return CTRLSEL_NONE;
	for (transfer = context->transfers; transfer != NULL; transfer = transfer->next)
		if (transfer->property == xpropev->atom &&
		    transfer->requestor == xpropev->window)
			goto found;
	return CTRLSEL_NONE;
found:
	if (transfer->size >= transfer->target->bufsize)
		transfer->size = transfer->target->bufsize;
	size = transfer->target->bufsize - transfer->size;
	if (size > context->selmaxsize)
		size = context->selmaxsize;
	XChangeProperty(
		xpropev->display,
		xpropev->window,
		xpropev->atom,
		transfer->target->target,
		transfer->target->format,
		PropModeReplace,
		transfer->target->buffer + transfer->size,
		size / nbytes(transfer->target->format)
	);
	if (transfer->size >= transfer->target->bufsize) {
		deltransfer(context, transfer);
	} else {
		transfer->size += size;
	}
	return CTRLSEL_INTERNAL;
}

int
ctrlsel_send(struct CtrlSelContext *context, XEvent *xev)
{
	int status;

	if (xev->type == SelectionRequest)
		status = sendinit(context, xev);
	else if (xev->type == SelectionClear)
		status = sendlost(context, xev);
	else if (xev->type == DestroyNotify)
		status = senddestroy(context, xev);
	else if (xev->type == PropertyNotify)
		status = sendincr(context, xev);
	else
		return CTRLSEL_NONE;
	if (status == CTRLSEL_LOST || status == CTRLSEL_ERROR)
		freetransferences(context);
	return status;
}

void
ctrlsel_cancel(struct CtrlSelContext *context)
{
	if (context->ndone < context->ntargets)
		freebuffers(context);
	freetransferences(context);
}

void
ctrlsel_disown(struct CtrlSelContext *context)
{
	freetransferences(context);
}
