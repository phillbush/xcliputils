#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>

static int
xerror(Display *display, XErrorEvent *e)
{
	char msg[128], number[128], req[128];

	if (e->error_code == BadWindow)
		return 0;
	XGetErrorText(display, e->error_code, msg, sizeof(msg));
	(void)snprintf(number, sizeof(number), "%d", e->request_code);
	XGetErrorDatabaseText(display, "XRequest", number, "<unknown>", req, sizeof(req));
	errx(EXIT_FAILURE, "%s (0x%08lX): %s", req, e->resourceid, msg);
	return 0;               /* unreachable */
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

int
xfork(void)
{
	pid_t pid;

	if ((pid = fork()) == -1) {
		warn("fork");
		return -1;
	}
	if (pid != 0)   /* parent */
		exit(EXIT_SUCCESS);
	return 0;
}

int
xinit(Display **display, Window *window)
{
	char buf[1];

	*display = NULL;
	*window = None;
	if ((*display = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		return 0;
	}
	/*
	 * We force reading of XErrorDB into memory so we can
	 * drop the "rpath" promise on pledge(2) on OpenBSD.
	 */
	(void)XGetErrorDatabaseText(*display, "XProtoError", "0", "", buf, 1);
	(void)XSetErrorHandler(xerror);
	if ((*window = createwindow(*display)) == None) {
		warnx("could not create manager window");
		return 0;
	}
	return 1;
}

void
xclose(Display *display, Window window)
{
	if (window != None)
		(void)XDestroyWindow(display, window);
	if (display != NULL)
		(void)XCloseDisplay(display);
}
