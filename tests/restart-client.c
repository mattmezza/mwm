#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>

int
main(int argc, char **argv)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy || argc != 2)
		return 1;

	int tray = !strcmp(argv[1], "tray");
	Window root = DefaultRootWindow(dpy);
	Window win = XCreateSimpleWindow(dpy, root, 40, 80, tray ? 18 : 500, tray ? 18 : 350, 0, 0, 0);
	XStoreName(dpy, win, argv[1]);
	if (tray) {
		Window owner = XGetSelectionOwner(dpy, XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False));
		if (!owner)
			return 2;
		XEvent event = {0};
		event.xclient = (XClientMessageEvent){
			.type = ClientMessage, .display = dpy, .window = owner,
			.message_type = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False),
			.format = 32, .data.l = {CurrentTime, 0, win},
		};
		XSendEvent(dpy, owner, False, NoEventMask, &event);
	} else {
		XMapWindow(dpy, win);
	}
	XSync(dpy, False);
	printf("0x%lx\n", win);
	fflush(stdout);
	for (;;) {
		XEvent event;
		XNextEvent(dpy, &event);
	}
}
