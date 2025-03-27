/*
 * ctrlsel: API to own/convert/transfer X11 selections
 * Refer to the accompanying manual for a description of the interface.
 */
#ifndef _CTRLSEL_H_
#define _CTRLSEL_H_

#include <stddef.h>

struct ctrlsel {
	void           *data;
	size_t          length;
	Atom            type;
	int             format;
};

int ctrlsel_request(
	Display        *display,
	Time            timestamp,
	Atom            selection,
	Atom            target,
	struct ctrlsel *content
);

Time ctrlsel_own(
	Display        *display,
	Window          owner,
	Time            timestamp,
	Atom            selection
);

int ctrlsel_answer(
	XEvent const   *event,
	Time            epoch,
	Atom const      targets[],
	size_t          ntargets,
	int             (*callback)(void *, Atom, struct ctrlsel *),
	void           *arg
);

#endif /* _CTRLSEL_H_ */
