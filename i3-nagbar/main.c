/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 * Converted from nagbar to oled-bar by Andrew Barrett-Sprot
 *
 * i3-oled-bar is a utility which slightly offsets the screen to reduce burn-in
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <stdint.h>
#include <getopt.h>
#include <limits.h>
#include <fcntl.h>
#include <paths.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_event.h>
#include <xcb/randr.h>
#include <xcb/xcb_cursor.h>

#include "libi3.h"
#include "i3-nagbar.h"

/** This is the equivalent of XC_left_ptr. I’m not sure why xcb doesn’t have a
 * constant for that. */
#define XCB_CURSOR_LEFT_PTR 68
static char *argv0 = NULL;
#define DEFAULT_MAX_OFFSET 200 // testing purposes

static int random_height;
static int max_offset;

typedef struct {
    i3String *label;
    char *action;
    int16_t x;
    uint16_t width;
} button_t;

static xcb_window_t top_win;
static xcb_rectangle_t top_rect = {0, 0, 600, 20};
static xcb_window_t bottom_win;
static xcb_rectangle_t bottom_rect = {0, 0, 600, 20};

/* Result of get_colorpixel() for the various colors. */
static color_t color_background;        /* background of the bar */

xcb_window_t root;
xcb_connection_t *conn;
xcb_screen_t *root_screen;

/*
 * Having verboselog(), errorlog() and debuglog() is necessary when using libi3.
 *
 */
void verboselog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}

void errorlog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void debuglog(char *fmt, ...) {
}

/*
 * Handles expose events (redraws of the window) and rendering in general. Will
 * be called from the code with event == NULL or from X with event != NULL.
 *
 */
static int handle_expose_top(xcb_connection_t *conn, xcb_expose_event_t *event) {
    /* re-draw the background */
    xcb_flush(conn);
    return 1;
}

static int handle_expose_bottom(xcb_connection_t *conn, xcb_expose_event_t *event) {
    xcb_flush(conn);
    return 1;
}

/**
 * Return the position and size the i3-nagbar window should use.
 * This will be the primary output or a fallback if it cannot be determined.
 */
static xcb_rectangle_t get_window_position(int height) {
    /* Default values if we cannot determine the primary output or its CRTC info. */
    xcb_rectangle_t result = (xcb_rectangle_t){50, 50, 500, height};

    xcb_randr_get_screen_resources_current_cookie_t rcookie = xcb_randr_get_screen_resources_current(conn, root);
    xcb_randr_get_output_primary_cookie_t pcookie = xcb_randr_get_output_primary(conn, root);

    xcb_randr_get_output_primary_reply_t *primary = NULL;
    xcb_randr_get_screen_resources_current_reply_t *res = NULL;

    if ((primary = xcb_randr_get_output_primary_reply(conn, pcookie, NULL)) == NULL) {
        DLOG("Could not determine the primary output.\n");
        goto free_resources;
    }

    if ((res = xcb_randr_get_screen_resources_current_reply(conn, rcookie, NULL)) == NULL) {
        goto free_resources;
    }

    xcb_randr_get_output_info_reply_t *output =
        xcb_randr_get_output_info_reply(conn,
                                        xcb_randr_get_output_info(conn, primary->output, res->config_timestamp),
                                        NULL);
    if (output == NULL || output->crtc == XCB_NONE)
        goto free_resources;

    xcb_randr_get_crtc_info_reply_t *crtc =
        xcb_randr_get_crtc_info_reply(conn,
                                      xcb_randr_get_crtc_info(conn, output->crtc, res->config_timestamp),
                                      NULL);
    if (crtc == NULL)
        goto free_resources;

    DLOG("Found primary output on position x = %i / y = %i / w = %i / h = %i.\n",
         crtc->x, crtc->y, crtc->width, crtc->height);
    if (crtc->width == 0 || crtc->height == 0) {
        DLOG("Primary output is not active, ignoring it.\n");
        goto free_resources;
    }

    result.x = crtc->x;
    result.y = crtc->y;
    goto free_resources;

free_resources:
    FREE(res);
    FREE(primary);
    return result;
}

int main(int argc, char *argv[]) {
    /* The following lines are a terribly horrible kludge. Because terminal
     * emulators have different ways of interpreting the -e command line
     * argument (some need -e "less /etc/fstab", others need -e less
     * /etc/fstab), we need to write commands to a script and then just run
     * that script. However, since on some machines, $XDG_RUNTIME_DIR and
     * $TMPDIR are mounted with noexec, we cannot directly execute the script
     * either.
     *
     * Initially, we tried to pass the command via the environment variable
     * _I3_NAGBAR_CMD. But turns out that some terminal emulators such as
     * xfce4-terminal run all windows from a single master process and only
     * pass on the command (not the environment) to that master process.
     *
     * Therefore, we symlink i3-nagbar (which MUST reside on an executable
     * filesystem) with a special name and run that symlink. When i3-nagbar
     * recognizes it’s started as a binary ending in .nagbar_cmd, it strips off
     * the .nagbar_cmd suffix and runs /bin/sh on argv[0]. That way, we can run
     * a shell script on a noexec filesystem.
     *
     * From a security point of view, i3-nagbar is just an alias to /bin/sh in
     * certain circumstances. This should not open any new security issues, I
     * hope. */
    char *cmd = NULL;
    const size_t argv0_len = strlen(argv[0]);
    if (argv0_len > strlen(".nagbar_cmd") &&
        strcmp(argv[0] + argv0_len - strlen(".nagbar_cmd"), ".nagbar_cmd") == 0) {
        unlink(argv[0]);
        cmd = sstrdup(argv[0]);
        *(cmd + argv0_len - strlen(".nagbar_cmd")) = '\0';
        execl("/bin/sh", "/bin/sh", cmd, NULL);
        err(EXIT_FAILURE, "execv(/bin/sh, /bin/sh, %s)", cmd);
    }

    argv0 = argv[0];

    int o, option_index = 0;

    static struct option long_options[] = {
        {"version", no_argument, 0, 'v'},
        {"tallness", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    char *options_string = "b:t:vh";

    srand(time(NULL));
    max_offset = DEFAULT_MAX_OFFSET;
    while ((o = getopt_long(argc, argv, options_string, long_options, &option_index)) != -1) {
        switch (o) {
            case 'v':
                printf("i3-oled-bar" I3_VERSION "\n");
                return 0;
            case 't': {// Tallness
                    char* endp = NULL;
                    if (!optarg)
                    {
                        fprintf(stderr, "invalid m option %s - expecting a number\n", optarg?optarg:"");
                        return 0;
                    }
                    max_offset = (int) strtoul(optarg, &endp, 10);
                }
                break;
            case 'h':
                printf("i3-oled-bar " I3_VERSION "\n");
                printf("i3-oled-bar  [-t tallness] [-v]\n");
                return 0;
        }
    }

    random_height = rand() % max_offset;

    int screens;
    if ((conn = xcb_connect(NULL, &screens)) == NULL ||
        xcb_connection_has_error(conn))
        die("Cannot open display\n");

/* Place requests for the atoms we need as soon as possible */
#define xmacro(atom) \
    xcb_intern_atom_cookie_t atom##_cookie = xcb_intern_atom(conn, 0, strlen(#atom), #atom);
#include "atoms.xmacro"
#undef xmacro

    root_screen = xcb_aux_get_screen(conn, screens);
    root = root_screen->root;

    // Set the colors
    color_background = draw_util_hex_to_color("#000000");

#if defined(__OpenBSD__)
    if (pledge("stdio rpath wpath cpath getpw proc exec", NULL) == -1)
        err(EXIT_FAILURE, "pledge");
#endif

    xcb_rectangle_t top_win_pos = get_window_position(random_height);
    xcb_rectangle_t bottom_win_pos = get_window_position(max_offset-random_height);

    xcb_cursor_t cursor;
    xcb_cursor_context_t *cursor_ctx;
    if (xcb_cursor_context_new(conn, root_screen, &cursor_ctx) == 0) {
        cursor = xcb_cursor_load_cursor(cursor_ctx, "left_ptr");
        xcb_cursor_context_free(cursor_ctx);
    } else {
        cursor = xcb_generate_id(conn);
        i3Font cursor_font = load_font("cursor", false);
        xcb_create_glyph_cursor(
            conn,
            cursor,
            cursor_font.specific.xcb.id,
            cursor_font.specific.xcb.id,
            XCB_CURSOR_LEFT_PTR,
            XCB_CURSOR_LEFT_PTR + 1,
            0, 0, 0,
            65535, 65535, 65535);
    }

    /* Open an input window */
    top_win = xcb_generate_id(conn);
    bottom_win = xcb_generate_id(conn);

    xcb_create_window(
        conn,
        XCB_COPY_FROM_PARENT,
        top_win,                                                 /* the window id */
        root,                                                /* parent == root */
        top_win_pos.x, top_win_pos.y, top_win_pos.width, top_win_pos.height, /* dimensions */
        0,                                                   /* x11 border = 0, we draw our own */
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_WINDOW_CLASS_COPY_FROM_PARENT, /* copy visual from parent */
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_CURSOR,
        (uint32_t[]){
            0, /* back pixel: black */
            XCB_EVENT_MASK_EXPOSURE |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                XCB_EVENT_MASK_BUTTON_PRESS |
                XCB_EVENT_MASK_BUTTON_RELEASE,
            cursor});

    xcb_create_window(
        conn,
        XCB_COPY_FROM_PARENT,
        bottom_win,                                                 /* the window id */
        root,                                                /* parent == root */
        bottom_win_pos.x, bottom_win_pos.y, bottom_win_pos.width, bottom_win_pos.height, /* dimensions */
        0,                                                   /* x11 border = 0, we draw our own */
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_WINDOW_CLASS_COPY_FROM_PARENT, /* copy visual from parent */
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_CURSOR,
        (uint32_t[]){
            0, /* back pixel: black */
            XCB_EVENT_MASK_EXPOSURE |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                XCB_EVENT_MASK_BUTTON_PRESS |
                XCB_EVENT_MASK_BUTTON_RELEASE,
            cursor});

    /* Map the window (make it visible) */
    xcb_map_window(conn, top_win);
    xcb_map_window(conn, bottom_win);

/* Setup NetWM atoms */
#define xmacro(name)                                                                       \
    do {                                                                                   \
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, name##_cookie, NULL); \
        if (!reply)                                                                        \
            die("Could not get atom " #name "\n");                                         \
                                                                                           \
        A_##name = reply->atom;                                                            \
        free(reply);                                                                       \
    } while (0);
#include "atoms.xmacro"
#undef xmacro

    /* Set dock mode */
    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        top_win,
                        A__NET_WM_WINDOW_TYPE,
                        A_ATOM,
                        32,
                        1,
                        (unsigned char *)&A__NET_WM_WINDOW_TYPE_DOCK);

    /* Set dock mode */
    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        bottom_win,
                        A__NET_WM_WINDOW_TYPE,
                        A_ATOM,
                        32,
                        1,
                        (unsigned char *)&A__NET_WM_WINDOW_TYPE_DOCK);

    /* Reserve some space at the top of the screen */
    struct __attribute__((__packed__)) partial{
        uint32_t left;
        uint32_t right;
        uint32_t top;
        uint32_t bottom;
        uint32_t left_start_y;
        uint32_t left_end_y;
        uint32_t right_start_y;
        uint32_t right_end_y;
        uint32_t top_start_x;
        uint32_t top_end_x;
        uint32_t bottom_start_x;
        uint32_t bottom_end_x;
    };
    struct partial top_strut_partial;
    struct partial bottom_strut_partial;
    memset(&top_strut_partial, 0, sizeof(top_strut_partial));
    memset(&bottom_strut_partial, 0, sizeof(bottom_strut_partial));

    top_strut_partial.top = 1;
    top_strut_partial.top_start_x = 0;
    top_strut_partial.top_end_x = 800;
    bottom_strut_partial.bottom = 1;
    bottom_strut_partial.bottom_start_x = 0;
    bottom_strut_partial.bottom_end_x = 800;

    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        top_win,
                        A__NET_WM_STRUT_PARTIAL,
                        A_CARDINAL,
                        32,
                        12,
                        &top_strut_partial);
    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        bottom_win,
                        A__NET_WM_STRUT_PARTIAL,
                        A_CARDINAL,
                        32,
                        12,
                        &bottom_strut_partial);

    /* Grab the keyboard to get all input */
    xcb_flush(conn);

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn)) != NULL) {
        if (event->response_type == 0) {
            fprintf(stderr, "X11 Error received! sequence %x\n", event->sequence);
            continue;
        }

        /* Strip off the highest bit (set if the event is generated) */
        int type = (event->response_type & 0x7F);

        switch (type) {
            case XCB_EXPOSE: {
                xcb_expose_event_t *expose = (xcb_expose_event_t *)event;
                if (expose->window == top_win) {
                    handle_expose_top(conn,expose);
                } else { // if (expose->window == bottom_win)
                    handle_expose_bottom(conn,expose);
                }
                break;
            }

            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *configure_notify = (xcb_configure_notify_event_t *)event;
                if (configure_notify->window == top_win) {
                    top_rect = (xcb_rectangle_t){
                        configure_notify->x,
                        configure_notify->y,
                        configure_notify->width,
                        configure_notify->height};
                } else { //if (configure_notify->window == bottom_win) {
                    bottom_rect = (xcb_rectangle_t){
                        configure_notify->x,
                        configure_notify->y,
                        configure_notify->width,
                        configure_notify->height};
                }
                break;
            }
        }

        free(event);
    }

    return 0;
}
