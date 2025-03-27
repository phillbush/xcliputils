#include <sys/stat.h>
#include <sys/mman.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <control/selection.h>

#include "util.h"

static int
callback(void *arg, Atom target, struct ctrlsel *content)
{
	(void)target;
	*content = *(struct ctrlsel *)arg;
	return 1;
}

static void
send_clip(char * const targetnames[], char const *data, size_t size)
{
	Display *display;
	Window owner;
	XEvent event;
	Atom selection;
	Atom polymorphic_type, string_type;
	Atom targets[32];       /* optimist maximum */
	Time epoch;
	size_t ntargets;
	int error;

	display = xinit();
	selection = getatom(display, SELECTION);
	if (size < 1) {
		ctrlsel_own(display, None, CurrentTime, selection);
		XCloseDisplay(display);
		return;
	}
	owner = createwindow(display);
	for (ntargets = 0; ntargets < LEN(targets) && targetnames[ntargets] != NULL; ntargets++)
		targets[ntargets] = getatom(display, targetnames[ntargets]);
	polymorphic_type = getatom(display, "TEXT");
	string_type = getatom(display, "STRING");
	if (ntargets == 0) {
		targets[ntargets++] = getatom(display, "UTF8_STRING");
		targets[ntargets++] = string_type;
		targets[ntargets++] = polymorphic_type;
		targets[ntargets++] = getatom(display, "COMPOUND_TEXT");
	}
	if ((epoch = ctrlsel_own(display, owner, CurrentTime, selection)) == 0)
		errx(EXIT_FAILURE, "could not own selection");
	daemonize();
	while (!XNextEvent(display, &event)) switch (event.type) {
	case SelectionClear:
		if (event.xselectionclear.window == owner)
			return;
		continue;
	case DestroyNotify:
		if (event.xdestroywindow.window == owner)
			return;
		continue;
	case SelectionRequest:
		if (event.xselectionrequest.selection != selection)
			continue;
		error = -ctrlsel_answer(
			&event, epoch, targets, ntargets,
			callback, (void *)&(struct ctrlsel){
				.data = (void *)data,
				.length = size,
				.format = 8,
				.type = targets[0] == polymorphic_type
					? string_type : targets[0],
			}
		);
		if (error)
			warnx("could not request selection: %s", strerror(error));
		continue;
	}
	XDestroyWindow(display, owner);
	XCloseDisplay(display);
}

int
main(int argc, char *argv[])
{
	struct stat stat;
	FILE *stream;
	char buf[BUFSIZ];
	char *data;
	ssize_t nread;
	size_t size;

	(void)argc;
	if (fstat(STDIN_FILENO, &stat) == -1)
		err(EXIT_FAILURE, "stat");
	data = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, STDIN_FILENO, 0);
	if (data != MAP_FAILED) {
		send_clip(argv+1, data, stat.st_size);
		munmap(data, stat.st_size);
	} else {
		stream = open_memstream(&data, &size);
		while ((nread = read(STDIN_FILENO, buf, sizeof(buf))) != 0) {
			if (nread == -1)
				err(EXIT_FAILURE, "read");
			if ((ssize_t)fwrite(buf, 1, nread, stream) != nread)
				err(EXIT_FAILURE, "fwrite");
		}
		(void)fclose(stream);
		send_clip(argv+1, data, size);
		free(data);
	}
	return EXIT_SUCCESS;
}
