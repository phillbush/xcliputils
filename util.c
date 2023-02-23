#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>

static int
xerror(Display *display, XErrorEvent *e)
{
	char buf[BUFSIZ];

	if (e->error_code == BadWindow)
		return 0;
	XGetErrorText(display, e->error_code, buf, BUFSIZ);
	errx(EXIT_FAILURE, "%lu: %s", e->resourceid, buf);
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
	*display = NULL;
	*window = None;
	if ((*display = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		return 0;
	}
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
