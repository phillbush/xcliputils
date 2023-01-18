#include <stdlib.h>
#include <string.h>
#include <err.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#define SELMAXSIZE      0x40000

enum {
	ATOM_PAIR,
	COMPOUND_TEXT,
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

enum {
	/* indices for the selections we can own */
	SEL_MANAGER,
	SEL_CLIPBOARD,
	SEL_PRIMARY,
	SEL_LAST,
};

enum {
	CONTENT_INCR,
	CONTENT_ERROR,
	CONTENT_SUCCESS,
};

struct Manager {
	Window win;

	/*
	 * We save the clipboard data into this buffer structure.
	 */
	struct Buffer {
		struct Buffer *next;
		size_t buflen;
		unsigned char *data;
	} *buffers;

	/*
	 * For each target, we save the converted data sent by the
	 * original clipboard owner into the .buf element.  If the
	 * content is sent incrementally, .incr is True.
	 */
	struct Target {
		struct Target *next;
		struct Buffer *buf;
		Atom target;
		int incr;
	} *targets;

	/*
	 * We need to keep track of the selection we can own; whether we
	 * own them; and the time we owned them.
	 */
	struct Selections {
		Atom atom;              /* the selection atom */
		Time time;              /* when we acquired the selection */
		int own;                /* whether we own the selection */
	} sels[SEL_LAST];

	/* whether we are running */
	int running;
};

static Atom atoms[ATOM_LAST];
static Display *dpy;
static Window root;
static int xfixes;
static unsigned long selmaxsize = 0;
static int (*xerrorxlib)(Display *, XErrorEvent *);
static char *atomnames[ATOM_LAST] = {
	[ATOM_PAIR]             = "ATOM_PAIR",
	[COMPOUND_TEXT]         = "COMPOUND_TEXT",
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
static int selids[SEL_LAST] = {
	[SEL_MANAGER]           = CLIPBOARD_MANAGER,
	[SEL_CLIPBOARD]         = CLIPBOARD,
	[SEL_PRIMARY]           = PRIMARY,
};

static void *
erealloc(void *p, size_t size)
{
	if ((p = realloc(p, size)) == NULL)
		err(1, "realloc");
	return p;
}

static void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(1, "malloc");
	return p;
}

static int
xerror(Display *dpy, XErrorEvent *e)
{
	if (e->error_code == BadWindow || e->error_code == BadAlloc)
		return 0;
	return xerrorxlib(dpy, e);
	exit(1);        /* unreached */
}

static Window
createwin(Display *dpy)
{
	return XCreateWindow(
		dpy, root,
		0, 0, 1, 1, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWEventMask,
		&(XSetWindowAttributes){
			.event_mask = StructureNotifyMask | PropertyChangeMask,
		}
	);
}

static unsigned long
getatomsprop(Display *dpy, Window win, Atom prop, Atom *type, Atom **atoms)
{
	unsigned char *p;
	unsigned long len;
	unsigned long dl;   /* dummy variable */
	int di;             /* dummy variable */
	int state;

	p = NULL;
	state = XGetWindowProperty(
		dpy, win,
		prop, 0L, 0x1FFFFFFF,
		False,
		*type, type,
		&di, &len, &dl, &p
	);
	if (state != Success || len == 0 || p == NULL) {
		*atoms = NULL;
		XFree(p);
		return 0;
	}
	*atoms = (Atom *)p;
	return len;
}

static unsigned long
getatompairs(Window win, Atom prop, Atom **pairs)
{
	unsigned char *p;
	unsigned long len;
	unsigned long dl;   /* dummy variable */
	int di;             /* dummy variable */
	Atom type;
	size_t size;

	if (XGetWindowProperty(dpy, win, prop, 0L, 0x1FFFFFFF, False, atoms[ATOM_PAIR], &type, &di, &len, &dl, &p) != Success ||
	    len == 0 || p == NULL || type != atoms[ATOM_PAIR]) {
		XFree(p);
		*pairs = NULL;
		return 0;
	}
	size = len * sizeof(**pairs);
	*pairs = emalloc(size);
	memcpy(*pairs, p, size);
	XFree(p);
	return len;
}

static Time
getservertime(Display *dpy, Window win)
{
	unsigned char *p;
	XEvent xev;

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

	XChangeProperty(
		dpy, win,
		atoms[_TIMESTAMP_PROP], atoms[_TIMESTAMP_PROP],
		8L, PropModeAppend, p, 0
	);
	while (!XNextEvent(dpy, &xev)) {
		if (xev.type == PropertyNotify &&
		    xev.xproperty.window == win &&
		    xev.xproperty.atom == atoms[_TIMESTAMP_PROP]) {
			return xev.xproperty.time;
		}
	}
	/* unreachable */
	errx(EXIT_FAILURE, "could not get server time");
	return CurrentTime;
}

static Time *
getselectiontimeptr(struct Manager *man, Atom selection)
{
	if (selection == XA_PRIMARY)
		return &man->sels[SEL_PRIMARY].time;
	if (selection == atoms[CLIPBOARD])
		return &man->sels[SEL_CLIPBOARD].time;
	return &man->sels[SEL_MANAGER].time;
}

static int
doweownselection(struct Manager *man, Atom selection)
{
	return (selection == XA_PRIMARY && man->sels[SEL_PRIMARY].own) ||
	       (selection == atoms[CLIPBOARD] && man->sels[SEL_CLIPBOARD].own) ||
	       (selection == atoms[CLIPBOARD_MANAGER]);
}

static void
ownselection(struct Manager *man, int selnum, Time time)
{
	XSetSelectionOwner(dpy, man->sels[selnum].atom, man->win, time);
	man->sels[selnum].time = time;
	man->sels[selnum].own = True;
}

static void
cleancontent(struct Manager *man)
{
	struct Buffer *buf;
	struct Target *tgt;

	while (man->buffers != NULL) {
		buf = man->buffers;
		man->buffers = man->buffers->next;
		free(buf->data);
		free(buf);
	}
	while (man->targets != NULL) {
		tgt = man->targets;
		man->targets = man->targets->next;
		free(tgt);
	}
}

static struct Buffer *
newbuf(struct Manager *man)
{
	struct Buffer *buf;

	buf = emalloc(sizeof(*buf));
	*buf = (struct Buffer){
		.next = man->buffers,
		.buflen = 0,
		.data = NULL,
	};
	man->buffers = buf;
	return buf;
}

static struct Target *
newtgt(struct Manager *man)
{
	struct Target *tgt;

	tgt = emalloc(sizeof(*tgt));
	*tgt = (struct Target){
		.next = man->targets,
		.buf = NULL,
		.target = None,
		.incr = False,
	};
	man->targets = tgt;
	return tgt;
}

static int
getcontent(struct Manager *man, Atom prop, struct Buffer **buf_ret)
{
	struct Buffer *buf;
	unsigned char *p;
	unsigned long len, size;
	unsigned long dl;   /* dummy variable */
	int format, status;
	Atom type;

	/*
	 * Get content of property and append it into buffer *buf_ret.
	 *
	 * If *buf_ret is NULL, save into it a new buffer.
	 *
	 * Return CONTENT_SUCCESS on success; CONTENT_ERROR on error;
	 * or CONTENT_INCR on incremental transfers.
	 */
	status = XGetWindowProperty(
		dpy, man->win,
		prop,
		0L, 0x1FFFFFFF,
		True,
		AnyPropertyType,
		&type,
		&format,
		&len, &dl, &p
	);
	if (status != Success || len == 0 || p == NULL) {
		XFree(p);
		return CONTENT_ERROR;
	}
	if (type == atoms[INCR]) {
		XFree(p);
		buf = newbuf(man);
		*buf_ret = buf;
		return CONTENT_INCR;
	}
	size = len;
	if (*buf_ret != NULL) {
		buf = *buf_ret;
		/* append buffer */
		size += buf->buflen;
		buf->data = erealloc(buf->data, size);
		memcpy(buf->data + buf->buflen, p, len);
		buf->buflen += len;
	} else {
		/* new buffer */
		for (buf = man->buffers; buf != NULL; buf = buf->next) {
			if (buf->buflen != len)
				continue;
			if (memcmp(buf->data, p, len) == 0) {
				*buf_ret = buf;
				goto done;
			}
		}
		buf = newbuf(man);
		buf->data = emalloc(size);
		buf->buflen = len;
		memcpy(buf->data, p, len);
		*buf_ret = buf;
	}
done:
	XFree(p);
	return CONTENT_SUCCESS;
}

static void
requestclipboard(struct Manager *man, Atom target, Time time)
{

	/* According to ICCCM, clients should ensure that the named
	 * property does not exist on the window before issuing the
	 * ConvertSelection request.  The exception to this rule is
	 * when the client intends to pass parameters with the request.
	 */
	XDeleteProperty(dpy, man->win, atoms[CLIPBOARD]);
	XConvertSelection(dpy, atoms[CLIPBOARD], target, target, man->win, time);
}

static void
clean(struct Manager *man)
{
	man->running = False;
	cleancontent(man);
	XDestroyWindow(dpy, man->win);
	XCloseDisplay(dpy);
}

static void
init(struct Manager *man)
{
	int i;

	/*
	 * According to ICCCM, a manager client (that is, a client
	 * responsible for managing shared resources, such as our
	 * clipboard manager) should take ownership of an appropriate
	 * selection (`CLIPBOARD_MANAGER`, in our case).
	 *
	 * Immediately after a manager successfully acquires ownership
	 * of a manager selection, it should announce its arrival by
	 * sending a `ClientMessage` event.  (That is necessary for
	 * clients to be able to know when a specific manager has
	 * started: any client that wish to do so should select for
	 * `StructureNotify` on the root window and should watch for
	 * the appropriate `MANAGER` `ClientMessage`).
	 *
	 * Return non-zero when ownership of the manager selection is
	 * successfully acquired; zero if we could not own it or if
	 * there is already another manager owning it.
	 */
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(EXIT_FAILURE, "could not open display");
	if (!XFixesQueryExtension(dpy, &xfixes, &i))
		errx(EXIT_FAILURE, "could not use XFixes");
	root = DefaultRootWindow(dpy);
	XInternAtoms(dpy, atomnames, ATOM_LAST, False, atoms);
	xerrorxlib = XSetErrorHandler(xerror);

	man->running = True;
	man->buffers = NULL;
	man->targets = NULL;
	for (i = 0; i < SEL_LAST; i++) {
		man->sels[i].atom = atoms[selids[i]];
		man->sels[i].own = False;
	}
	if ((man->win = createwin(dpy)) == None) {
		XCloseDisplay(dpy);
		errx(EXIT_FAILURE, "could not create window");
	}
	if (XGetSelectionOwner(dpy, atoms[CLIPBOARD_MANAGER]) != None) {
		clean(man);
		errx(EXIT_FAILURE, "there's already a clipboard manager running");
	}
	ownselection(man, SEL_MANAGER, getservertime(dpy, man->win));
	if (XGetSelectionOwner(dpy, atoms[CLIPBOARD_MANAGER]) != man->win) {
		clean(man);
		errx(EXIT_FAILURE, "could not own manager selection");
	}
	XFixesSelectSelectionInput(dpy, man->win, atoms[CLIPBOARD], XFixesSetSelectionOwnerNotifyMask);
	XSendEvent(
		dpy, root,
		False,
		StructureNotifyMask,
		(XEvent *)&(XClientMessageEvent){
			.type         = ClientMessage,
			.window       = root,
			.message_type = atoms[MANAGER],
			.format       = 32,
			.data.l[0]    = man->sels[SEL_MANAGER].time,         /* timestamp */
			.data.l[1]    = atoms[CLIPBOARD_MANAGER], /* manager selection atom */
			.data.l[2]    = man->win,                 /* window owning the selection */
			.data.l[3]    = 0,                        /* manager-specific data */
			.data.l[4]    = 0,                        /* manager-specific data */
		}
	);
	if ((selmaxsize = XExtendedMaxRequestSize(dpy)) == 0 || (selmaxsize = XMaxRequestSize(dpy)) == 0) {
		clean(man);
		errx(EXIT_FAILURE, "could not get maximum request size");
	}
	if (selmaxsize > SELMAXSIZE)
		selmaxsize = SELMAXSIZE;
	ownselection(man, SEL_CLIPBOARD, man->sels[SEL_MANAGER].time);
	return;
}

static Bool
savetargets(struct Manager *man, Window requestor, Atom *targets, unsigned long nitems, Atom property, Time time)
{
	unsigned long len;
	unsigned long dl;   /* dummy variable */
	unsigned long nout, i;
	int format, status;
	Atom atom, type;
	Atom *pairs;
	unsigned char *p;

	p = NULL;
	if (nitems == 0) {
		status = XGetWindowProperty(
			dpy, requestor, property,
			0L, 0x1FFFFFFF,
			False,
			XA_ATOM, &type,
			&format, &len, &dl,
			(unsigned char **)&p
		);
		if (status != Success || len == 0 || p == NULL) {
			XFree(p);
			return False;
		}
		targets = (Atom *)p;
		nitems = len;
	}
	pairs = emalloc(2 * nitems * sizeof(*pairs));
	nout = 0;
	for (i = 0; i < nitems; i++) {
		atom = targets[i];
		if (atom == atoms[ATOM_PAIR]
		|| atom == atoms[COMPOUND_TEXT]
		|| atom == atoms[CLIPBOARD]
		|| atom == atoms[CLIPBOARD_MANAGER]
		|| atom == atoms[DELETE]
		|| atom == atoms[INCR]
		|| atom == atoms[MANAGER]
		|| atom == atoms[MULTIPLE]
		|| atom == atoms[_NULL]
		|| atom == atoms[PRIMARY]
		|| atom == atoms[SAVE_TARGETS]
		|| atom == atoms[TARGETS]
		|| atom == atoms[TIMESTAMP]
		|| atom == atoms[_TIMESTAMP_PROP])
			continue;
		pairs[nout++] = atom;
		pairs[nout++] = atom;
	}
	XFree(p);
	XChangeProperty(
		dpy,
		man->win,
		atoms[MULTIPLE],
		atoms[ATOM_PAIR],
		32,
		PropModeReplace,
		(unsigned char *)pairs,
		nout
	);
	free(pairs);
	XConvertSelection(
		dpy,
		atoms[CLIPBOARD],
		atoms[MULTIPLE],
		atoms[MULTIPLE],
		man->win,
		time
	);
	return True;
}

static struct Target *
gettarget(struct Manager *man, Atom target)
{
	struct Target *tgt;

	for (tgt = man->targets; tgt != NULL; tgt = tgt->next)
		if (target == tgt->target)
			break;
	return tgt;
}

static Bool
convert(struct Manager *man, Window requestor, Atom selection, Atom target, Atom property, Time time)
{
	struct Target *tgt;
	struct Buffer *buf;
	int ntargets;
	Atom *targets;

	if (target == atoms[MULTIPLE]) {
		/*
		 * A MULTIPLE should be handled when processing a
		 * SelectionRequest event.  We do not support nested
		 * MULTIPLE targets.
		 */
		return False;
	}
	if (target == atoms[TIMESTAMP]) {
		/*
		 * According to ICCCM, to avoid some race conditions, it
		 * is important that requestors be able to discover the
		 * timestamp the owner used to acquire ownership.
		 * Requester do that by requesting sellection owners to
		 * convert to `TIMESTAMP`.  Selections owners must
		 * return the timestamp as an `XA_INTEGER`.
		 */
		XChangeProperty(
			dpy,
			requestor,
			property,
			XA_INTEGER, 32,
			PropModeReplace,
			(unsigned char *)getselectiontimeptr(man, selection),
			1
		);
		return True;
	}
	if (target == atoms[TARGETS]) {
		/*
		 * According to ICCCM, when requested for the `TARGETS`
		 * target, the selection owner should return a list of
		 * atoms representing the targets for which an attempt
		 * to convert the selection will (hopefully) succeed.
		 *
		 * The list of supported atoms for conversion of the
		 * manager selection (`CLIPBOARD_MANAGER`) and the
		 * actual content selections (`CLIPBOARD` and `PRIMARY`)
		 * are different.
		 */
		ntargets = 0;
		if (selection == atoms[CLIPBOARD_MANAGER]) {
			targets = emalloc(3 * sizeof(*targets));
			targets[ntargets++] = atoms[TIMESTAMP];
			targets[ntargets++] = atoms[SAVE_TARGETS];
			targets[ntargets++] = atoms[TARGETS];
		} else {
			for (tgt = man->targets; tgt != NULL; tgt = tgt->next)
				ntargets++;
			targets = emalloc((ntargets + 3) * sizeof(*targets));
			ntargets = 0;
			targets[ntargets++] = atoms[TIMESTAMP];
			targets[ntargets++] = atoms[DELETE];
			targets[ntargets++] = atoms[MULTIPLE];
			for (tgt = man->targets; tgt != NULL; tgt = tgt->next) {
				targets[ntargets++] = tgt->target;
			}
		}
		XChangeProperty(
			dpy,
			requestor,
			property,
			XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)targets,
			ntargets
		);
		free(targets);
		return True;
	}
	if (selection == atoms[CLIPBOARD_MANAGER] && target == atoms[SAVE_TARGETS]) {
		return savetargets(man, requestor, NULL, 0, property, time);
	}
	if (selection == atoms[CLIPBOARD_MANAGER] || target == atoms[SAVE_TARGETS]) {
		/*
		 * We do not handle other targets for the manager
		 * selection.  We also do not handle SAVE_TARGETS
		 * target for non-manager selections.
		 */
		return False;
	}
	if (target == atoms[DELETE]) {
		cleancontent(man);
		XChangeProperty(
			dpy,
			requestor,
			property,
			atoms[_NULL],
			8L,
			PropModeReplace,
			(unsigned char *)"",
			0
		);
		return True;
	}
	if (target == atoms[TEXT] ||
	    target == atoms[UTF8_STRING] ||
	    target == atoms[COMPOUND_TEXT] ||
	    target == XA_STRING) {
	    target = atoms[UTF8_STRING];
	}
	if ((tgt = gettarget(man, target)) == NULL)
		return False;
	if ((buf = tgt->buf) == NULL)
		return False;
	XChangeProperty(
		dpy,
		requestor,
		property,
		target,
		8L,
		PropModeReplace,
		(buf->data == NULL ? (unsigned char *)"" : buf->data),
		buf->buflen
	);
	return True;
}

static void
destroynotify(XEvent *p, struct Manager *man)
{
	XDestroyWindowEvent *xev;

	xev = &p->xdestroywindow;
	if (xev->window == man->win) {
		/* someone destroyed our window; stop running */
		man->running = False;
	}
}

static void
selclear(XEvent *p, struct Manager *man)
{
	XSelectionClearEvent *xev;

	xev = &p->xselectionclear;
	if (xev->selection == atoms[CLIPBOARD_MANAGER]) {
		/* we lost the manager selection; stop running */
		man->running = False;
	} else if (xev->selection == atoms[CLIPBOARD]) {
		/*
		 * We lost the clipboard.  We could request its content
		 * now; but we do not.
		 *
		 * We use XFixes to be also notified when the clipboard
		 * is owned by another application.  We are therefore
		 * notified twice: once because we lost the clipboard,
		 * and another because someone else owned it.  We must
		 * request the clipboard on only one of those events.
		 *
		 * It is better to request when we are notified by
		 * XFixes when someone else owned the clipboard, because
		 * someone may own the clipboard when we have not owned
		 * it previously.
		 */
	}
}

static void
selnotify(XEvent *p, struct Manager *man)
{
	XSelectionEvent *xev;
	struct Buffer *buf;
	struct Target *tgt;
	unsigned long ntargets, i;
	Atom *targets;
	Atom type;
	int status;

	xev = &p->xselection;
	if (xev->selection != atoms[CLIPBOARD])
		return;
	if (xev->requestor != man->win)
		return;
	if (man->sels[SEL_CLIPBOARD].own) {
		/* We already own the clipboard */
		return;
	}
	if (xev->target == atoms[TARGETS] && xev->property == None) {
		/*
		 * We requested to the original clipboard owner the targets it
		 * supports.  We got no answer; fallback to XA_STRING.
		 */
		requestclipboard(man, XA_STRING, xev->time);
	} else if (xev->property == atoms[TARGETS]) {
		targets = None;
		type = XA_ATOM;
		ntargets = getatomsprop(xev->display, xev->requestor, xev->property, &type, &targets);
		if (ntargets == 0 || targets == NULL) {
			requestclipboard(man, XA_STRING, xev->time);
		} else {
			savetargets(man, xev->requestor, targets, ntargets, None, xev->time);
		}
		XFree(targets);
	} else if (xev->property == atoms[MULTIPLE]) {
		targets = None;
		type = atoms[ATOM_PAIR];
		ntargets = getatomsprop(xev->display, xev->requestor, xev->property, &type, &targets);
		if (ntargets == 0 || targets == NULL) {
			XFree(targets);
			requestclipboard(man, XA_STRING, xev->time);
			return;
		}
		ntargets /= 2;
		for (i = 0; i < ntargets; i += 2) {
			if (targets[i] == None || targets[i+1] == None)
				continue;
			buf = NULL;
			if ((status = getcontent(man, targets[i], &buf)) == CONTENT_ERROR) {
				if (buf != NULL) {
					free(buf->data);
					free(buf);
				}
				continue;
			}
			tgt = newtgt(man);
			tgt->target = targets[i+1];
			tgt->incr = (status == CONTENT_INCR);
			tgt->buf = buf;
		}
		XFree(targets);
		ownselection(man, SEL_CLIPBOARD, xev->time);
		ownselection(man, SEL_PRIMARY, xev->time);
	} else if (xev->property == XA_STRING) {
		/* conversion succeeded; get clipboard content and own selection */
		buf = NULL;
		if (getcontent(man, XA_STRING, &buf) != CONTENT_SUCCESS) {
			if (buf != NULL) {
				free(buf->data);
				free(buf);
			}
			return;
		}
		if (buf == NULL)
			return;
		tgt = newtgt(man);
		tgt->target = atoms[UTF8_STRING];
		tgt->buf = buf;
		tgt = newtgt(man);
		tgt->target = atoms[TEXT];
		tgt->buf = buf;
		tgt = newtgt(man);
		tgt->target = XA_STRING;
		tgt->buf = buf;
		ownselection(man, SEL_CLIPBOARD, xev->time);
		ownselection(man, SEL_PRIMARY, xev->time);
	} else {
		/* failed conversion */
	}
}

static void
selrequest(XEvent *p, struct Manager *man)
{
	enum { PAIR_TARGET, PAIR_PROPERTY, PAIR_LAST };
	XSelectionRequestEvent *xev;
	XSelectionEvent sev;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	unsigned long natoms, i;
	Bool success;

	xev = &p->xselectionrequest;
	sev = (XSelectionEvent){
		.type           = SelectionNotify,
		.display        = xev->display,
		.requestor      = xev->requestor,
		.selection      = xev->selection,
		.time           = xev->time,
		.target         = xev->target,
		.property       = None,
	};
	if (xev->time != CurrentTime && xev->time < man->sels[SEL_MANAGER].time) {
		/*
		 * According to ICCCM, the selection owner should
		 * compare the timestamp with the period it has owned
		 * the selection and, if the time is outside, refuse the
		 * `SelectionRequest` by sending the requestor window a
		 * `SelectionNotify` event with the property set to
		 * `None` (by means of a `SendEvent` request with an
		 * empty event mask).
		 */
		goto done;
	}
	if (!doweownselection(man, xev->selection)) {
		/*
		 * deal with the unlikely case when we got a
		 * `SelectionRequest` from a selection we do not own.
		 * (is that possible?)
		 */
		goto done;
	}
	if (xev->target == atoms[MULTIPLE]) {
		if (xev->property == None)
			goto done;
		natoms = getatompairs(xev->requestor, xev->property, &pairs);
	} else {
		pair[PAIR_TARGET] = xev->target;
		pair[PAIR_PROPERTY] = xev->property;
		pairs = pair;
		natoms = 2;
	}
	success = True;
	for (i = 0; i < natoms; i += 2) {
		if (!convert(man, xev->requestor, xev->selection,
		             pairs[i + PAIR_TARGET],
		             pairs[i + PAIR_PROPERTY],
		             xev->time)) {
			success = False;
			pairs[i + PAIR_PROPERTY] = None;
		}
	}
	if (xev->target == atoms[MULTIPLE]) {
		XChangeProperty(
			xev->display,
			xev->requestor,
			xev->property,
			atoms[ATOM_PAIR],
			32, PropModeReplace,
			(unsigned char *)pairs, natoms
		);
		free(pairs);
	}
	if (success) {
		sev.property = (xev->property == None) ? xev->target : xev->property;
	}
done:
	XSendEvent(xev->display, xev->requestor, False, NoEventMask, (XEvent *)&sev);
}

static void
propnotify(XEvent *p, struct Manager *man)
{
	XPropertyEvent *xev;
	struct Target *tgt;
	struct Buffer *buf;

	xev = &p->xproperty;
	if (xev->state == PropertyNewValue) {
		if ((tgt = gettarget(man, xev->atom)) == NULL)
			return;
		buf = tgt->buf;
		if (buf == NULL)
			return;
		if (!tgt->incr)
			return;
		(void)getcontent(man, xev->atom, &buf);
	}
}

static void
selfixes(XEvent *p, struct Manager *man)
{
	XFixesSelectionNotifyEvent *xev;

	xev = (XFixesSelectionNotifyEvent *)p;
	if (xev->selection == atoms[CLIPBOARD] && xev->owner != man->win) {
		/* another client got the clipboard; request its content */
		man->sels[SEL_CLIPBOARD].own = False;
		man->sels[SEL_PRIMARY].own = False;
		cleancontent(man);
		requestclipboard(man, atoms[TARGETS], xev->timestamp);
	}
}

int
main(void)
{
	static void (*xevents[LASTEvent])(XEvent *, struct Manager *) = {
		[DestroyNotify]         = destroynotify,
		[SelectionClear]        = selclear,
		[SelectionNotify]       = selnotify,
		[SelectionRequest]      = selrequest,
		[PropertyNotify]        = propnotify,
	};
	struct Manager man;
	XEvent xev;

	init(&man);
	while (man.running && !XNextEvent(dpy, &xev)) {
		if (xev.type == xfixes + XFixesSelectionNotify) {
			selfixes(&xev, &man);
		} else if (xevents[xev.type] != NULL) {
			(*xevents[xev.type])(&xev, &man);
		}
	}
	clean(&man);
	return EXIT_SUCCESS;
}
