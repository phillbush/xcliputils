#define LEN(a)   (sizeof(a) / sizeof((a)[0]))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

void daemonize(void);
Display *xinit(void);
Window createwindow(Display *display);
Atom getatom(Display *display, char const *atomname);
Time getservertime(Display *display);
