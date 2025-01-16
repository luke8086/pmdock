/*
 * pmdock - An X11 panel for hosting dockapps and app launchers
 *
 * Copyright (C) 2024-2025 luke8086 <luke8086@fastmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <Imlib2.h>

#ifndef DEFAULT_BG_PATH
#define DEFAULT_BG_PATH "tile-default.png"
#endif

struct size {
    unsigned width;
    unsigned height;
};

struct position {
    int x;
    int y;
};

#define TILE_TYPE_APP 0
#define TILE_TYPE_LAUNCHER 1

struct tile {
    const char *command;
    Imlib_Image icon;
    pid_t pid;
    const char *res_name;
    unsigned type;
    Window window;
};

struct app {
    int above_all;
    int all_desktops;
    Imlib_Image bg_image;
    int daemon_mode;
    Display *display;
    Window dock_window;
    int horizontal;
    int initial_x;
    int initial_y;
    unsigned long mwm_decor;
    unsigned long mwm_funcs;
    Window root_window;
    int screen;
    struct tile *tiles;
    unsigned tile_count;
    unsigned tile_size;
    int verbose;
};

static void pm_fprintf(FILE *, const char *, const char *, va_list);
static void pm_debug(const char *, ...);
static void pm_warn(const char *, ...);
static void pm_error(const char *, ...);
static void pm_assert(int, const char *, ...);
static void exit_usage(int);
static int check_window_manager(void);
static Window get_icon_window(Window);
static Window get_icon_window_waiting(Window);
static struct size get_window_size(Window);
static void set_wm_class_hint(Window, const char *, const char *);
static void set_mwm_hints(Window, unsigned long, unsigned long, unsigned long);
static void set_wm_desktop_hint(Window, int32_t);
static void set_wm_above_hint(Window);
static struct position get_tile_position(unsigned);
static int check_all_dockapps_swallowed(void);
static void swallow_dockapp(Window, int);
static void handle_sigusr1(int);
static void handle_sigterm(int);
static int handle_error_event(Display *, XErrorEvent *);
static int handle_io_error_event(Display *);
static void handle_create_event(const XEvent *);
static void handle_button_press_event(const XEvent *);
static void handle_expose_event(Window);
static void parse_opts(int, char *[]);
static void daemonize(void);
static void setup_display(void);
static void create_dock_window(void);
static void create_launchers(void);
static void start_dockapps(void);
static void terminate_dockapps(void);

// clang-format off
static const char USAGE[] =
    "Usage: pmdock [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  -a            Show on all virtual desktops\n"
    "  -A            Show on top of all windows\n"
    "  -x POSITION   X coordinate (default: 0)\n"
    "  -y POSITION   Y coordinate (default: 0)\n"
    "  -s SIZE       Tile size in pixels (default: 64)\n"
    "  -b IMAGE      Tile background image (default: tile-default.png)\n"
    "  -H            Use horizontal layout\n"
    "  -D DECOR      Window decorations hints (default: 0x00)\n"
    "  -f FUNCS      Window functions hints (default: 0x00)\n"
    "  -d            Daemonize after swallowing all dockapps\n"
    "  -r NAME       Resource name for dockapp in the next tile\n"
    "  -i ICON       Icon path for launcher in the next tile\n"
    "  -c COMMAND    Command to execute in the next tile\n"
    "  -t TYPE       Add tile (dockapp or launcher)\n"
    "  -v            Show debug messages\n"
    "  -h            Display this help message\n";
// clang-format on

static struct app app = {
    .above_all = 0,
    .all_desktops = 0,
    .bg_image = NULL,
    .daemon_mode = 0,
    .display = NULL,
    .dock_window = None,
    .horizontal = 0,
    .initial_x = 0,
    .initial_y = 0,
    .mwm_decor = 0,
    .mwm_funcs = 0,
    .root_window = None,
    .screen = 0,
    .tiles = NULL,
    .tile_count = 0,
    .tile_size = 64,
    .verbose = 0,
};

static void
pm_fprintf(FILE *f, const char *prefix, const char *fmt, va_list args)
{
    fprintf(f, "%s", prefix);
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
}

static void
pm_debug(const char *fmt, ...)
{
    if (!app.verbose) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    pm_fprintf(stderr, "pmdock (DEBUG): ", fmt, args);
    va_end(args);
}

static void
pm_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    pm_fprintf(stderr, "pmdock (WARNING): ", fmt, args);
    va_end(args);
}

static void
pm_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    pm_fprintf(stderr, "pmdock (ERROR): ", fmt, args);
    va_end(args);
}

static void
pm_assert(int condition, const char *fmt, ...)
{
    if (!condition) {
        va_list args;
        va_start(args, fmt);
        pm_fprintf(stderr, "pmdock (ERROR): ", fmt, args);
        va_end(args);

        exit(1);
    }
}

static void
exit_usage(int status)
{
    fprintf(stderr, "%s", USAGE);
    exit(status);
}

static int
check_window_manager(void)
{
    Atom net_supporting_wm_check = XInternAtom(app.display, "_NET_SUPPORTING_WM_CHECK", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(app.display, app.root_window, net_supporting_wm_check,
        0, 1, False, XA_WINDOW, &actual_type,
        &actual_format, &nitems, &bytes_after, &data);

    if (status == Success && data) {
        XFree(data);
        return 1;
    }

    return 0;
}

static Window
get_icon_window(Window window)
{
    XWMHints *wm_hints = XGetWMHints(app.display, window);
    Window ret = None;

    if (wm_hints == NULL) {
        return None;
    }

    if (wm_hints->flags & IconWindowHint) {
        ret = wm_hints->icon_window;
    }

    XFree(wm_hints);

    return ret;
}

static Window
get_icon_window_waiting(Window window)
{
    Window ret = None;

    for (int i = 0; i < 2; ++i) {
        ret = get_icon_window(window);

        if (ret != None) {
            break;
        }

        pm_debug("Waiting for icon window of 0x%lx", window);
        usleep(100000);
    }

    return ret;
}

static struct size
get_window_size(Window window)
{
    Window root;
    unsigned border, depth;
    struct size ret = { 0, 0 };
    int x, y;

    if (XGetGeometry(app.display, window, &root, &x, &y, &ret.width, &ret.height, &border, &depth) == 0) {
        pm_debug("Failed to get geometry of window 0x%lx", window);
        return ret;
    }

    pm_debug("Window 0x%lx has size %ux%u, border %u, depth %u", window, ret.width, ret.height, border, depth);

    return ret;
}

static void
set_wm_class_hint(Window window, const char *res_name, const char *res_class)
{
    XClassHint *class_hint = XAllocClassHint();
    pm_assert(class_hint != NULL, "Failed to allocate class hint");

    class_hint->res_name = (char *)res_name;
    class_hint->res_class = (char *)res_class;

    XSetClassHint(app.display, window, class_hint);
    XFree(class_hint);
}

static void
set_mwm_hints(Window window, unsigned long flags, unsigned long funcs, unsigned long decor)
{
    Atom motif_hints_atom = XInternAtom(app.display, "_MOTIF_WM_HINTS", False);
    unsigned long hints[5] = { flags, funcs, decor, 0, 0 };

    XChangeProperty(app.display, window, motif_hints_atom, motif_hints_atom, 32,
        PropModeReplace, (unsigned char *)&hints, 5);
}

static void
set_wm_desktop_hint(Window window, int32_t value)
{
    Atom net_wm_desktop = XInternAtom(app.display, "_NET_WM_DESKTOP", False);
    XChangeProperty(app.display, window, net_wm_desktop, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&value, 1);

    pm_debug("Set _NET_WM_DESKTOP hint for window 0x%lx to %d", window, value);
}

static void
set_wm_above_hint(Window window)
{
    Atom net_wm_state = XInternAtom(app.display, "_NET_WM_STATE", False);
    Atom net_wm_state_above = XInternAtom(app.display, "_NET_WM_STATE_ABOVE", False);

    XChangeProperty(app.display, window, net_wm_state, XA_ATOM, 32,
        PropModeReplace, (unsigned char *)&net_wm_state_above, 1);

    pm_debug("Set _NET_WM_STATE_ABOVE hint for window 0x%lx", window);
}

static struct position
get_tile_position(unsigned index)
{
    return (struct position) {
        .x = app.horizontal ? index * app.tile_size : 0,
        .y = app.horizontal ? 0 : index * app.tile_size
    };
}

static int
check_all_dockapps_swallowed(void)
{
    for (unsigned i = 0; i < app.tile_count; i++) {
        if (app.tiles[i].type == TILE_TYPE_APP && app.tiles[i].window == 0) {
            return 0;
        }
    }

    return 1;
}

void
swallow_dockapp(Window main_window, int index)
{
    pm_debug("Swallowing dockapp with main window 0x%lx at index %d", main_window, index);

    int wm_running = check_window_manager();

    if (wm_running) {
        pm_warn("Window manager detected, swallowing dockapp with workaround");

        // Give the WM time to handle the new window
        usleep(100000);
    }

    Window icon_window = get_icon_window_waiting(main_window);

    if (icon_window == None) {
        pm_warn("Window 0x%lx has no icon window, skipping", main_window);
        return;
    }

    app.tiles[index].window = icon_window;

    XSetWindowBorderWidth(app.display, icon_window, 0);

    struct size size = get_window_size(icon_window);
    struct position tile_pos = get_tile_position(index);
    int icon_x = tile_pos.x + (app.tile_size - size.width) / 2;
    int icon_y = tile_pos.y + (app.tile_size - size.height) / 2;
    int main_x = app.horizontal ? icon_x : (int)app.tile_size * 2;
    int main_y = app.horizontal ? (int)app.tile_size * 2 : icon_y;

    if (wm_running) {
        // Unmap/reparent windows and give the WM time to process it.
        XUnmapWindow(app.display, main_window);
        XUnmapWindow(app.display, icon_window);
        XFlush(app.display);
        usleep(50000);
        XReparentWindow(app.display, main_window, app.dock_window, main_x, main_y);
        XReparentWindow(app.display, icon_window, app.dock_window, icon_x, icon_y);
        XFlush(app.display);
        usleep(50000);
    }

    XReparentWindow(app.display, main_window, app.dock_window, main_x, main_y);
    XReparentWindow(app.display, icon_window, app.dock_window, icon_x, icon_y);
    XMapRaised(app.display, main_window);
    XMapRaised(app.display, icon_window);
    XFlush(app.display);

    pm_debug("Swallowed window 0x%lx at %ux%u", icon_window, icon_x, icon_y);

    if (check_all_dockapps_swallowed()) {
        pm_debug("All dockapps swallowed");

        XSelectInput(app.display, app.root_window, 0);

        if (app.daemon_mode) {
            kill(getppid(), SIGUSR1);
        }
    }
}

static void
handle_sigusr1(int signo)
{
    (void)signo; // Unused parameter

    pm_debug("Exiting parent process");

    exit(0);
}

static void
handle_sigterm(int signo)
{
    (void)signo; // Unused parameter

    terminate_dockapps();

    exit(0);
}

static int
handle_error_event(Display *display, XErrorEvent *error)
{
    char error_text[256];

    // Ignore BadWindow errors
    if (error->request_code == 20) {
        return 0;
    }

    XGetErrorText(display, error->error_code, error_text, sizeof(error_text));

    pm_debug("X11 Error (%d, %d, 0x%lx): %s",
        error->request_code, error->minor_code, error->resourceid, error_text);

    return 0;
}

static int
handle_io_error_event(Display *display)
{
    (void)display;

    pm_debug("X11 IO Error");

    terminate_dockapps();

    exit(1);
}

static void
handle_create_event(const XEvent *event)
{
    Window window = event->xcreatewindow.window;
    XClassHint class_hint;

    if (!XGetClassHint(app.display, window, &class_hint)) {
        return;
    }

    pm_debug("Created window 0x%lx with res_name '%s'", window, class_hint.res_name);

    for (unsigned i = 0; i < app.tile_count; ++i) {
        if (app.tiles[i].window == 0 && app.tiles[i].res_name && !strcmp(class_hint.res_name, app.tiles[i].res_name)) {
            swallow_dockapp(window, i);
            break;
        }
    }

    XFree(class_hint.res_name);
    XFree(class_hint.res_class);
}

static void
handle_button_press_event(const XEvent *event)
{
    for (unsigned i = 0; i < app.tile_count; i++) {
        if (app.tiles[i].type == TILE_TYPE_LAUNCHER && event->xbutton.window == app.tiles[i].window) {
            unsigned pid = fork();

            if (pid == 0) {
                execl("/bin/sh", "/bin/sh", "-c", app.tiles[i].command, (char *)NULL);
                pm_assert(0, "Failed to execute %s", app.tiles[i].command);
            }

            break;
        }
    }
}

static void
handle_expose_event(Window window)
{
    for (unsigned i = 0; i < app.tile_count; i++) {
        struct position pos = get_tile_position(i);

        if (window == app.dock_window && app.tiles[i].type != TILE_TYPE_LAUNCHER) {
            imlib_context_set_drawable(app.dock_window);
            imlib_context_set_image(app.bg_image);
            imlib_render_image_on_drawable(pos.x, pos.y);

            continue;
        }

        if (window == app.tiles[i].window && app.tiles[i].type == TILE_TYPE_LAUNCHER) {
            imlib_context_set_image(app.bg_image);
            imlib_context_set_drawable(app.tiles[i].window);
            imlib_render_image_on_drawable(0, 0);

            imlib_context_set_image(app.tiles[i].icon);
            unsigned width = imlib_image_get_width();
            unsigned height = imlib_image_get_height();
            int x = width < app.tile_size ? (app.tile_size - width) / 2 : 0;
            int y = height < app.tile_size ? (app.tile_size - height) / 2 : 0;
            imlib_render_image_on_drawable(x, y);

            continue;
        }
    }
}

static void
parse_opts(int argc, char *argv[])
{
    int opt;
    const char *pending_command = NULL;
    const char *pending_icon = NULL;
    const char *pending_resname = NULL;
    const char *bg_path = DEFAULT_BG_PATH;
    unsigned current_tile = 0;
    const char *optstring = "aAb:c:D:df:Hhi:s:t:r:vx:y:";

    // First pass: count tiles
    optind = 1;
    while ((opt = getopt(argc, argv, optstring)) != -1) {
        if (opt == 't') {
            ++app.tile_count;
        }
    }

    if (app.tile_count > 0) {
        app.tiles = calloc(app.tile_count, sizeof(struct tile));
        pm_assert(app.tiles != NULL, "Failed to allocate memory");
    }

    // Second pass: process arguments
    optind = 1;
    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
        case 'A':
            app.above_all = 1;
            break;
        case 'a':
            app.all_desktops = 1;
            break;
        case 'v':
            app.verbose = 1;
            break;
        case 'x':
            app.initial_x = atoi(optarg);
            break;
        case 'y':
            app.initial_y = atoi(optarg);
            break;
        case 's': {
            int size = atoi(optarg);
            if (size <= 0) {
                pm_error("Invalid tile size: %s", optarg);
                exit_usage(1);
            }
            app.tile_size = size;
            break;
        }
        case 'H':
            app.horizontal = 1;
            break;
        case 'r':
            pending_resname = optarg;
            break;
        case 'i':
            pending_icon = optarg;
            break;
        case 'c':
            pending_command = optarg;
            break;
        case 't':
            if (!pending_command) {
                pm_error("Error: -t requires preceding -c to specify command");
                exit_usage(1);
            }

            if (strcmp(optarg, "dockapp") == 0) {
                if (!pending_resname) {
                    pm_error("Error: dockapp type requires preceding -r to specify resource name");
                    exit_usage(1);
                }

                app.tiles[current_tile].type = TILE_TYPE_APP;
                app.tiles[current_tile].command = pending_command;
                app.tiles[current_tile].res_name = pending_resname;
            } else if (strcmp(optarg, "launcher") == 0) {
                if (!pending_icon) {
                    pm_error("Error: launcher type requires preceding -i to specify icon");
                    exit_usage(1);
                }

                app.tiles[current_tile].type = TILE_TYPE_LAUNCHER;
                app.tiles[current_tile].command = pending_command;
                app.tiles[current_tile].icon = imlib_load_image(pending_icon);
                pm_assert(app.tiles[current_tile].icon != NULL, "Failed to load icon %s", pending_icon);
            } else {
                pm_error("Error: invalid type '%s' (must be 'dockapp' or 'launcher')", optarg);
                exit_usage(1);
            }

            current_tile++;

            pending_command = NULL;
            pending_icon = NULL;
            pending_resname = NULL;

            break;
        case 'b':
            bg_path = optarg;
            break;
        case 'd':
            app.daemon_mode = 1;
            break;
        case 'f':
            app.mwm_funcs = strtoul(optarg, NULL, 0);
            break;
        case 'D':
            app.mwm_decor = strtoul(optarg, NULL, 0);
            break;
        case 'h':
            exit_usage(0);
            break;
        default:
            break;
        }
    }

    if (app.tile_count == 0) {
        pm_error("No tiles specified");
        exit_usage(1);
    }

    app.bg_image = imlib_load_image(bg_path);
    pm_assert(app.bg_image != NULL, "Failed to load background image: %s", bg_path);
}

static void
daemonize(void)
{
    pid_t pid = fork();
    pm_assert(pid >= 0, "Failed to fork");

    if (pid > 0) {
        // Parent process

        signal(SIGUSR1, handle_sigusr1);
        pause();

        pm_assert(0, "Parent process should not reach this point");
    }

    // Child process

    pm_assert(setsid() >= 0, "Failed to create new session");

    close(STDIN_FILENO);

    int fd = open("/dev/null", O_RDWR);
    pm_assert(fd >= 0, "Failed to open /dev/null");

    dup2(fd, STDIN_FILENO);

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    pm_debug("Daemonized child process %d", getpid());
}

static void
setup_display(void)
{
    app.display = XOpenDisplay(NULL);
    pm_assert(app.display != NULL, "Cannot open display");

    XSetErrorHandler(handle_error_event);
    XSetIOErrorHandler(handle_io_error_event);

    app.screen = DefaultScreen(app.display);
    app.root_window = RootWindow(app.display, app.screen);

    XSelectInput(app.display, app.root_window, SubstructureNotifyMask);

    imlib_context_set_display(app.display);
    imlib_context_set_visual(DefaultVisual(app.display, DefaultScreen(app.display)));
    imlib_context_set_colormap(DefaultColormap(app.display, DefaultScreen(app.display)));
}

static void
create_dock_window(void)
{
    unsigned width = app.horizontal ? app.tile_count * app.tile_size : app.tile_size;
    unsigned height = app.horizontal ? app.tile_size : app.tile_count * app.tile_size;
    int x = app.initial_x;
    int y = app.initial_y;

    app.dock_window = XCreateSimpleWindow(app.display, app.root_window,
        x, y, width, height, 0,
        BlackPixel(app.display, app.screen), WhitePixel(app.display, app.screen));

    XStoreName(app.display, app.dock_window, "PMDock");

    set_mwm_hints(app.dock_window, 0x03, app.mwm_funcs, app.mwm_decor);
    set_wm_class_hint(app.dock_window, "pmdock", "PMDock");

    if (app.above_all) {
        set_wm_above_hint(app.dock_window);
    }

    if (app.all_desktops) {
        set_wm_desktop_hint(app.dock_window, -1);
    }

    XMapWindow(app.display, app.dock_window);
    XMoveResizeWindow(app.display, app.dock_window, x, y, width, height);

    XSelectInput(app.display, app.dock_window, ExposureMask | StructureNotifyMask);

    pm_debug("Created dock window 0x%lx at %ux%u+%d+%d", app.dock_window, width, height, x, y);
}

static void
create_launchers(void)
{
    for (unsigned i = 0; i < app.tile_count; i++) {
        if (app.tiles[i].type != TILE_TYPE_LAUNCHER) {
            continue;
        }

        struct position pos = get_tile_position(i);

        Window win = XCreateSimpleWindow(app.display, app.dock_window,
            pos.x, pos.y, app.tile_size, app.tile_size, 0,
            BlackPixel(app.display, app.screen),
            WhitePixel(app.display, app.screen));

        app.tiles[i].window = win;

        XSelectInput(app.display, win, ExposureMask | ButtonPressMask);
        XMapWindow(app.display, win);

        pm_debug("Created launcher window 0x%lx at %ux%u", win, pos.x, pos.y);
    }
}

static void
start_dockapps(void)
{
    for (unsigned i = 0; i < app.tile_count; i++) {
        if (app.tiles[i].type != TILE_TYPE_APP) {
            continue;
        }

        app.tiles[i].pid = fork();
        pm_assert(app.tiles[i].pid >= 0, "Failed to fork");

        if (app.tiles[i].pid == 0) {
            execl("/bin/sh", "/bin/sh", "-c", app.tiles[i].command, (char *)NULL);
            exit(1);
        }

        pm_debug("Started dockapp %s with pid %d", app.tiles[i].command, app.tiles[i].pid);
    }
}

static void
terminate_dockapps(void)
{
    pm_debug("Terminating dockapps");

    for (unsigned i = 0; i < app.tile_count; i++) {
        if (app.tiles[i].pid > 0) {
            kill(app.tiles[i].pid, SIGTERM);
        }
    }
}

int
main(int argc, char *argv[])
{
    XEvent event;

    parse_opts(argc, argv);

    if (app.daemon_mode) {
        daemonize();
        // Now we're in the child process
    }

    signal(SIGTERM, handle_sigterm);

    setup_display();
    create_dock_window();
    create_launchers();

    XFlush(app.display);

    start_dockapps();

    while (1) {
        XNextEvent(app.display, &event);

        switch (event.type) {
        case CreateNotify:
            handle_create_event(&event);
            break;
        case Expose:
            handle_expose_event(event.xexpose.window);
            break;
        case ButtonPress:
            handle_button_press_event(&event);
            break;
        }
    }

    // NOTREACHED
    return 1;
}
