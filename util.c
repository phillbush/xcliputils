#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "util.h"

static int
xerror(Display *display, XErrorEvent *e)
{
	char msg[128], number[128], req[128];

	if (e->error_code == BadWindow || e->error_code == BadAtom)
		return 0;
	XGetErrorText(display, e->error_code, msg, sizeof(msg));
	(void)snprintf(number, sizeof(number), "%d", e->request_code);
	XGetErrorDatabaseText(display, "XRequest", number, "<unknown>", req, sizeof(req));
	errx(EXIT_FAILURE, "%s (0x%08lX): %s", req, e->resourceid, msg);
	/* unreachable */
	return 0;
}

static void
epledge(char const *promises)
{
	(void)promises;
#if __OpenBSD__
	int pledge(const char[], const char[]);

	if (pledge(promises, NULL) == -1)
		err(EXIT_FAILURE, "pledge");
#endif
}

Window
createwindow(Display *display)
{
	Window window;

	window = XCreateWindow(
		display,
		DefaultRootWindow(display),
		0, 0, 1, 1, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWEventMask,
		&(XSetWindowAttributes){
			.event_mask = StructureNotifyMask | PropertyChangeMask,
		}
	);
	if (window == None)
		errx(EXIT_FAILURE, "could not create window");
	return window;
}

void
daemonize(void)
{
	pid_t pid;

	if ((pid = fork()) == -1)
		err(EXIT_FAILURE, "fork");
	if (pid != 0)   /* parent */
		exit(EXIT_SUCCESS);
	if (setsid() == -1)
		err(EXIT_FAILURE, "setsid");
	epledge("stdio");
}

Display *
xinit(void)
{
	Display *display;
	char const *dpyname;
	char buf[1];

	dpyname = XDisplayName(NULL);
	if (dpyname == NULL || dpyname[0] == '\0')
		errx(EXIT_FAILURE, "DISPLAY is not set");
	if ((display = XOpenDisplay(dpyname)) == NULL)
		errx(EXIT_FAILURE, "%s: could not open display", dpyname);
	/*
	 * We force reading of XErrorDB into memory so we can
	 * drop the "rpath" promise on pledge(2) on OpenBSD.
	 */
	(void)XGetErrorDatabaseText(display, "XProtoError", "0", "", buf, 1);
	(void)XSetErrorHandler(xerror);
	epledge("stdio proc");
	return display;
}

Atom
getatom(Display *display, char const *atomname)
{
	Atom atom;

	if ((atom = XInternAtom(display, atomname, False)) == None) {
		errx(
			EXIT_FAILURE,
			"could not intern atom: %s",
			atomname
		);
	}
	return atom;
}

Time
getservertime(Display *display)
{
	XEvent event;
	Window window;

	/*
	 * To get the server time, we append a zero-length data to a
	 * window's property (any can do), and get the timestamp from
	 * the server in the corresponding XPropertyEvent(3).
	 *
	 * We create (and then delete) a window for that, to not mess
	 * with the mask of events selected on any existing window by
	 * the client.
	 */
	if ((window = createwindow(display)) == None)
		return CurrentTime;
	(void)XChangeProperty(
		display, window,
		XA_WM_NAME, XA_STRING,
		8L, PropModeAppend,
		(void *)"", 0   /* zero-length data */
	);
	(void)XWindowEvent(display, window, PropertyChangeMask, &event);
	(void)XDestroyWindow(display, window);
	return event.xproperty.time;
}
