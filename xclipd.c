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

struct Manager {
	struct Content {
		size_t bufsize, buflen;
		unsigned char *buf;
	} content;
	struct {
		Atom atom;      /* the selection atom */
		Time time;      /* when we acquired the selection */
		int own;        /* whether we own the selection */
	} sels[SEL_LAST];
	Window win;
	int running;            /* whether we are running */
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

static unsigned long
max(unsigned long x, unsigned long y)
{
	return x > y ? x : y;
}

static unsigned long
min(unsigned long x, unsigned long y)
{
	return x < y ? x : y;
}

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
cleanbuffer(struct Manager *man)
{
	if (man->content.buf == NULL)
		return;
	man->content.buf[0] = '\0';
	man->content.buflen = 0;
}

static void
getclipboard(struct Manager *man, Atom prop)
{
	unsigned char *p;
	unsigned long len, size;
	unsigned long dl;   /* dummy variable */
	int format;
	Atom type;

	if (XGetWindowProperty(dpy, man->win, prop, 0L, 0x1FFFFFFF, True, AnyPropertyType, &type, &format, &len, &dl, &p) != Success || len == 0 || p == NULL) {
		XFree(p);
		man->content.buflen = 0;
		return;
	}
	if (type == atoms[INCR]) {
		/* we do not do incremental transfer */
		XFree(p);
		cleanbuffer(man);
		return;
	}
	size = min(len + 1, selmaxsize);
	len = max(1, size) - 1;
	if (man->content.bufsize < size) {
		man->content.buf = erealloc(man->content.buf, size);
		man->content.bufsize = size;
	}
	man->content.buflen = len;
	memcpy(man->content.buf, p, man->content.buflen);
	man->content.buf[len] = '\0';
	XFree(p);
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
	free(man->content.buf);
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
	for (i = 0; i < SEL_LAST; i++) {
		man->sels[i].atom = atoms[selids[i]];
		man->sels[i].own = False;
	}
	man->content = (struct Content){
		.bufsize = 0,
		.buflen = 0,
		.buf = NULL
	};

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
savetargets(struct Manager *man, Window requestor, Atom property, Time time)
{
	unsigned long len, i;
	unsigned long dl;   /* dummy variable */
	int format, status, besttarget;
	Bool ret;
	Atom type, *targets;

	status = XGetWindowProperty(
		dpy, requestor, property,
		0L, 0x1FFFFFFF,
		False,
		XA_ATOM, &type,
		&format, &len, &dl,
		(unsigned char **)&targets
	);
	if (status != Success || len == 0 || targets == NULL) {
		XFree(targets);
		return False;
	}
	besttarget = None;
	for (i = 0; i < len; i++) {
		if (targets[i] == atoms[UTF8_STRING]) {
			besttarget = targets[i];
			break;
		} else if (targets[i] == atoms[TEXT] || targets[i] == XA_STRING) {
			besttarget = targets[i];
		}
	}
	ret = False;
	if (targets[i] != None) {
		requestclipboard(man, besttarget, time);
		XChangeProperty(
			dpy,
			requestor,
			property,
			atoms[_NULL],
			8L,
			PropModeReplace,
			"",
			0
		);
		ret = True;
	}
	XFree(targets);
	return ret;
}

static Bool
convert(struct Manager *man, Window requestor, Atom selection, Atom target, Atom property, Time time)
{
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
		 * are different.  That's why we have a conditional
		 * operator here.
		 */
		XChangeProperty(
			dpy,
			requestor,
			property,
			XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)(selection == atoms[CLIPBOARD_MANAGER]
			? (Atom[]){
				atoms[SAVE_TARGETS],
				atoms[TARGETS],
				atoms[TIMESTAMP],
			}
			: (Atom[]){
				XA_STRING,
				atoms[DELETE],
				atoms[MULTIPLE],
				atoms[TARGETS],
				atoms[TEXT],
				atoms[TIMESTAMP],
				atoms[UTF8_STRING],
			}),
#define NTARGETS_MANAGER 3
#define NTARGETS_CONTENT 7
			(selection == atoms[CLIPBOARD_MANAGER] ? NTARGETS_MANAGER : NTARGETS_CONTENT)
		);
		return True;
	}
	if (selection == atoms[CLIPBOARD_MANAGER] && target == atoms[SAVE_TARGETS]) {
		return savetargets(man, requestor, property, time);
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
		cleanbuffer(man);
		XChangeProperty(
			dpy,
			requestor,
			property,
			atoms[_NULL],
			8L,
			PropModeReplace,
			"",
			0
		);
		return True;
	}
	if (target == atoms[TEXT] ||
	    target == atoms[UTF8_STRING] ||
	    target == atoms[COMPOUND_TEXT] ||
	    target == XA_STRING) {
		XChangeProperty(
			dpy,
			requestor,
			property,
			atoms[UTF8_STRING],
			8L,
			PropModeReplace,
			(man->content.buf == NULL ? (unsigned char *)"" : man->content.buf),
			man->content.buflen
		);
		return True;
	}
	return False;
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
		/* we lost the clipboard; request its content */
		man->sels[SEL_CLIPBOARD].own = False;
		man->sels[SEL_PRIMARY].own = False;
		requestclipboard(man, atoms[UTF8_STRING], xev->time);
	}
}

static void
selnotify(XEvent *p, struct Manager *man)
{
	XSelectionEvent *xev;

	xev = &p->xselection;
	if (xev->property != None) {
		/* conversion succeeded; get clipboard content and own selection */
		getclipboard(man, xev->property);
		ownselection(man, SEL_CLIPBOARD, xev->time);
		ownselection(man, SEL_PRIMARY, xev->time);
	} else if (xev->target == atoms[UTF8_STRING]) {
		/* conversion to UTF8_STRING failed; try COMPOUND_TEXT */
		requestclipboard(man, atoms[COMPOUND_TEXT], xev->time);
	} else if (xev->target == atoms[COMPOUND_TEXT]) {
		/* conversion to COMPOUND_TEXT failed; try SRING */
		requestclipboard(man, XA_STRING, xev->time);
	} else if (xev->target == XA_STRING) {
		/* conversion to STRING failed; try TEXT */
		requestclipboard(man, atoms[TEXT], xev->time);
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
selfixes(XEvent *p, struct Manager *man)
{
	XFixesSelectionNotifyEvent *xev;

	xev = (XFixesSelectionNotifyEvent *)p;
	if (xev->selection == atoms[CLIPBOARD] && xev->owner != man->win) {
		/* another client got the clipboard; request its content */
		man->sels[SEL_CLIPBOARD].own = False;
		man->sels[SEL_PRIMARY].own = False;
		requestclipboard(man, atoms[UTF8_STRING], xev->timestamp);
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
