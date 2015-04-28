#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "../gui.h"

/* macros */
#define MAX_BUFFER  64
#define MAX_MEMORY  (8 * 1024)
#define MAX_PANELS  4
#define WIN_WIDTH   800
#define WIN_HEIGHT  600
#define DTIME       16

#define MIN(a,b)    ((a) < (b) ? (a) : (b))
#define MAX(a,b)    ((a) < (b) ? (b) : (a))
#define CLAMP(i,v,x) (MAX(MIN(v,x), i))
#define LEN(a)      (sizeof(a)/sizeof(a)[0])
#define UNUSED(a)   ((void)(a))

typedef struct XFont XFont;
typedef struct XSurface XSurface;
typedef struct XWindow XWindow;

struct XFont {
    int ascent;
    int descent;
    int height;
    XFontSet set;
    XFontStruct *xfont;
};

struct XSurface {
    GC gc;
    Display *dpy;
    int screen;
    Window root;
    Drawable drawable;
    unsigned int w, h;
    gui_size clip_depth;
};

struct XWindow {
    Display *dpy;
    Window root;
    Visual *vis;
    Colormap cmap;
    XWindowAttributes attr;
    XSetWindowAttributes swa;
    Window win;
    int screen;
    unsigned int width;
    unsigned int height;
};

struct demo {
    gui_char in_buf[MAX_BUFFER];
    gui_size in_len;
    gui_bool in_act;
    gui_bool check;
    gui_int option;
    gui_float slider;
    gui_size prog;
    gui_int spinner;
    gui_bool spin_act;
    gui_size item_cur;
    gui_size cur;
    gui_bool tab_min;
    gui_float group_off;
    gui_float shelf_off;
    gui_bool toggle;
};

static void
die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static void*
xcalloc(size_t siz, size_t n)
{
    void *ptr = calloc(siz, n);
    if (!ptr) die("Out of memory\n");
    return ptr;
}

static long
timestamp(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0) return 0;
    return (long)((long)tv.tv_sec * 1000 + (long)tv.tv_usec/1000);
}

static void
sleep_for(long t)
{
    struct timespec req;
    const time_t sec = (int)(t/1000);
    const long ms = t - (sec * 1000);
    req.tv_sec = sec;
    req.tv_nsec = ms * 1000000L;
    while(-1 == nanosleep(&req, &req));
}

static XFont*
font_create(Display *dpy, const char *name)
{
    int n;
    char *def, **missing;
    XFont *font = xcalloc(1, sizeof(XFont));
    font->set = XCreateFontSet(dpy, name, &missing, &n, &def);
    if(missing) {
        while(n--)
            fprintf(stderr, "missing fontset: %s\n", missing[n]);
        XFreeStringList(missing);
    }

    if(font->set) {
        XFontStruct **xfonts;
        char **font_names;
        XExtentsOfFontSet(font->set);
        n = XFontsOfFontSet(font->set, &xfonts, &font_names);
        while(n--) {
            font->ascent = MAX(font->ascent, (*xfonts)->ascent);
            font->descent = MAX(font->descent,(*xfonts)->descent);
            xfonts++;
        }
    } else {
        if(!(font->xfont = XLoadQueryFont(dpy, name))
        && !(font->xfont = XLoadQueryFont(dpy, "fixed")))
            die("error, cannot load font: '%s'\n", name);
        font->ascent = font->xfont->ascent;
        font->descent = font->xfont->descent;
    }
    font->height = font->ascent + font->descent;
    return font;
}

static gui_size
font_get_text_width(void *handle, const gui_char *text, gui_size len)
{
    XFont *font = handle;
    XRectangle r;
    gui_size width;
    if(!font || !text)
        return 0;

    if(font->set) {
        XmbTextExtents(font->set, (const char*)text, (int)len, NULL, &r);
        return r.width;
    }
    else {
        return (gui_size)XTextWidth(font->xfont, (const char*)text, (int)len);
    }
    return width;
}

static void
font_del(Display *dpy, XFont *font)
{
    if(!font) return;
    if(font->set)
        XFreeFontSet(dpy, font->set);
    else
        XFreeFont(dpy, font->xfont);
    free(font);
}

static unsigned long
color_from_byte(struct gui_color col)
{
    unsigned long res = 0;
    res |= (unsigned long)col.r << 16;
    res |= (unsigned long)col.g << 8;
    res |= (unsigned long)col.b << 0;
    return (res);
}

static XSurface*
surface_create(Display *dpy,  int screen, Window root, unsigned int w, unsigned int h)
{
    XSurface *surface = xcalloc(1, sizeof(XSurface));
    surface->w = w;
    surface->h = h;
    surface->dpy = dpy;
    surface->screen = screen;
    surface->root = root;
    surface->gc = XCreateGC(dpy, root, 0, NULL);
    XSetLineAttributes(dpy, surface->gc, 1, LineSolid, CapButt, JoinMiter);
    surface->drawable = XCreatePixmap(dpy, root, w, h, (unsigned int)DefaultDepth(dpy, screen));
    return surface;
}

static void
surface_resize(XSurface *surf, unsigned int w, unsigned int h) {
    if(!surf) return;
    if (surf->w == w && surf->h == h) return;
    surf->w = w; surf->h = h;
    if(surf->drawable) XFreePixmap(surf->dpy, surf->drawable);
    surf->drawable = XCreatePixmap(surf->dpy, surf->root, w, h,
        (unsigned int)DefaultDepth(surf->dpy, surf->screen));
}

static void
surface_scissor(XSurface *surf, float x, float y, float w, float h)
{
    XRectangle clip_rect;
    clip_rect.x = (short)x;
    clip_rect.y = (short)y;
    clip_rect.width = (unsigned short)w;
    clip_rect.height = (unsigned short)h;
    clip_rect.width = (unsigned short)MIN(surf->w, clip_rect.width);
    clip_rect.height = (unsigned short)MIN(surf->h, clip_rect.height);
    XSetClipRectangles(surf->dpy, surf->gc, 0, 0, &clip_rect, 1, Unsorted);
}

static void
surface_draw_line(XSurface *surf, gui_short x0, gui_short y0, gui_short x1,
    gui_short y1, struct gui_color col)
{
    unsigned long c = color_from_byte(col);
    XSetForeground(surf->dpy, surf->gc, c);
    XDrawLine(surf->dpy, surf->drawable, surf->gc, (int)x0, (int)y0, (int)x1, (int)y1);
}

static void
surface_draw_rect(XSurface* surf, gui_short x, gui_short y, gui_ushort w,
    gui_ushort h, struct gui_color col)
{
    unsigned long c = color_from_byte(col);
    XSetForeground(surf->dpy, surf->gc, c);
    XFillRectangle(surf->dpy, surf->drawable, surf->gc, (int)x, (int)y, (unsigned)w, (unsigned)h);
}

static void
surface_draw_triangle(XSurface *surf, gui_short x0, gui_short y0, gui_short x1,
    gui_short y1, gui_short x2, gui_short y2, struct gui_color col)
{
    XPoint pnts[3];
    unsigned long c = color_from_byte(col);
    pnts[0].x = (short)x0;
    pnts[0].y = (short)y0;
    pnts[1].x = (short)x1;
    pnts[1].y = (short)y1;
    pnts[2].x = (short)x2;
    pnts[2].y = (short)y2;
    XSetForeground(surf->dpy, surf->gc, c);
    XFillPolygon(surf->dpy, surf->drawable, surf->gc, pnts, 3, Convex, CoordModeOrigin);
}

static void
surface_draw_circle(XSurface *surf, gui_short x, gui_short y, gui_ushort w,
    gui_ushort h, struct gui_color col)
{
    unsigned long c = color_from_byte(col);
    XSetForeground(surf->dpy, surf->gc, c);
    XFillArc(surf->dpy, surf->drawable, surf->gc, (int)x, (int)y,
        (unsigned)w, (unsigned)h, 0, 360 * 64);
}

static void
surface_draw_text(XSurface *surf, gui_short x, gui_short y, gui_ushort w, gui_ushort h,
    const char *text, size_t len, XFont *font, struct gui_color cbg, struct gui_color cfg)
{
    int i, tx, ty, th, olen;
    unsigned long bg = color_from_byte(cbg);
    unsigned long fg = color_from_byte(cfg);

    XSetForeground(surf->dpy, surf->gc, bg);
    XFillRectangle(surf->dpy, surf->drawable, surf->gc, (int)x, (int)y, (unsigned)w, (unsigned)h);
    if(!text || !font || !len) return;

    tx = (int)x;
    th = font->ascent + font->descent;
    ty = (int)y + ((int)h / 2) - (th / 2) + font->ascent;
    XSetForeground(surf->dpy, surf->gc, fg);
    if(font->set)
        XmbDrawString(surf->dpy,surf->drawable,font->set,surf->gc,tx,ty,(const char*)text,(int)len);
    else
        XDrawString(surf->dpy, surf->drawable, surf->gc, tx, ty, (const char*)text, (int)len);
}

static void
surface_clear(XSurface *surf, unsigned long color)
{
    XSetForeground(surf->dpy, surf->gc, color);
    XFillRectangle(surf->dpy, surf->drawable, surf->gc, 0, 0, surf->w, surf->h);
}

static void
surface_blit(Drawable target, XSurface *surf, unsigned int width, unsigned int height)
{
    XCopyArea(surf->dpy, surf->drawable, target, surf->gc, 0, 0, width, height, 0, 0);
}

static void
surface_del(XSurface *surf)
{
    XFreePixmap(surf->dpy, surf->drawable);
    XFreeGC(surf->dpy, surf->gc);
    free(surf);
}

static void
draw(XSurface *surf, struct gui_command_list *list)
{
    struct gui_command *cmd;
    if (!list->count) return;
    cmd = list->begin;
    while (cmd != list->end) {
        switch (cmd->type) {
        case GUI_COMMAND_NOP: break;
        case GUI_COMMAND_SCISSOR: {
            struct gui_command_scissor *s = (void*)cmd;
            surface_scissor(surf, s->x, s->y, s->w, s->h);
        } break;
        case GUI_COMMAND_LINE: {
            struct gui_command_line *l = (void*)cmd;
            surface_draw_line(surf, l->begin[0], l->begin[1], l->end[0],
                l->end[1], l->color);
        } break;
        case GUI_COMMAND_RECT: {
            struct gui_command_rect *r = (void*)cmd;
            surface_draw_rect(surf, r->x, r->y, r->w, r->h, r->color);
        } break;
        case GUI_COMMAND_CIRCLE: {
            struct gui_command_circle *c = (void*)cmd;
            surface_draw_circle(surf, c->x, c->y, c->w, c->h, c->color);
        } break;
        case GUI_COMMAND_TRIANGLE: {
            struct gui_command_triangle *t = (void*)cmd;
            surface_draw_triangle(surf, t->a[0], t->a[1], t->b[0], t->b[1],
                t->c[0], t->c[1], t->color);
        } break;
        case GUI_COMMAND_TEXT: {
            struct gui_command_text *t = (void*)cmd;
            surface_draw_text(surf, t->x, t->y, t->w, t->h, (const char*)t->string,
                    t->length, t->font, t->bg, t->fg);
        } break;
        default: break;
        }
        cmd = cmd->next;
    }

}

static void
key(struct XWindow *xw, struct gui_input *in, XEvent *evt, gui_bool down)
{
    int ret;
    KeySym *code = XGetKeyboardMapping(xw->dpy, (KeyCode)evt->xkey.keycode, 1, &ret);
    if (*code == XK_Control_L || *code == XK_Control_R)
        gui_input_key(in, GUI_KEY_CTRL, down);
    else if (*code == XK_Shift_L || *code == XK_Shift_R)
        gui_input_key(in, GUI_KEY_SHIFT, down);
    else if (*code == XK_Delete)
        gui_input_key(in, GUI_KEY_DEL, down);
    else if (*code == XK_Return)
        gui_input_key(in, GUI_KEY_ENTER, down);
    else if (*code == XK_space)
        gui_input_key(in, GUI_KEY_SPACE, down);
    else if (*code == XK_BackSpace)
        gui_input_key(in, GUI_KEY_BACKSPACE, down);
    else if (*code > 32 && *code < 128 && !down) {
        gui_glyph glyph;
        glyph[0] = (gui_char)*code;
        gui_input_char(in, glyph);
    }
    XFree(code);
}

static void
motion(struct gui_input *in, XEvent *evt)
{
    const gui_int x = evt->xmotion.x;
    const gui_int y = evt->xmotion.y;
    gui_input_motion(in, x, y);
}

static void
btn(struct gui_input *in, XEvent *evt, gui_bool down)
{
    const gui_int x = evt->xbutton.x;
    const gui_int y = evt->xbutton.y;
    if (evt->xbutton.button == Button1)
        gui_input_button(in, x, y, down);
}

static void
resize(struct XWindow *xw, XSurface *surf)
{
    XGetWindowAttributes(xw->dpy, xw->win, &xw->attr);
    xw->width = (unsigned int)xw->attr.width;
    xw->height = (unsigned int)xw->attr.height;
    surface_resize(surf, xw->width, xw->height);
}

static void
demo_panel(struct gui_panel_layout *panel, struct demo *demo)
{
    gui_int i = 0;
    enum {HISTO, PLOT};
    const char *shelfs[] = {"Histogram", "Lines"};
    const gui_float values[] = {8.0f, 15.0f, 20.0f, 12.0f, 30.0f};
    const char *items[] = {"Fist", "Pistol", "Shotgun", "Railgun", "BFG"};
    const char *options[] = {"easy", "normal", "hard", "hell", "doom", "godlike"};
    struct gui_panel_layout tab;

    /* Tabs */
    demo->tab_min = gui_panel_tab_begin(panel, &tab, "Difficulty", demo->tab_min);
    gui_panel_row(&tab, 30, 3);
    for (i = 0; i < (gui_int)LEN(options); i++) {
        if (gui_panel_option(&tab, options[i], demo->option == i))
            demo->option = i;
    }
    gui_panel_tab_end(panel, &tab);

    /* Shelf */
    gui_panel_row(panel, 200, 2);
    demo->cur = gui_panel_shelf_begin(panel,&tab,shelfs,LEN(shelfs),demo->cur,demo->shelf_off);
    gui_panel_row(&tab, 100, 1);
    if (demo->cur == HISTO) {
        gui_panel_histo(&tab, values, LEN(values));
    } else {
        gui_panel_plot(&tab, values, LEN(values));
    }
    demo->shelf_off = gui_panel_shelf_end(panel, &tab);

    /* Group */
    gui_panel_group_begin(panel, &tab, "Options", demo->group_off);
    gui_panel_row(&tab, 30, 1);
    if (gui_panel_button_text(&tab, "button", GUI_BUTTON_DEFAULT))
        fprintf(stdout, "button pressed!\n");
    demo->toggle = gui_panel_button_toggle(&tab, "toggle", demo->toggle);
    demo->check = gui_panel_check(&tab, "advanced", demo->check);
    demo->slider = gui_panel_slider(&tab, 0, demo->slider, 10, 1.0f);
    demo->prog = gui_panel_progress(&tab, demo->prog, 100, gui_true);
    demo->item_cur = gui_panel_selector(&tab, items, LEN(items), demo->item_cur);
    demo->spinner = gui_panel_spinner(&tab, 0, demo->spinner, 250, 10, &demo->spin_act);
    demo->in_len = gui_panel_input(&tab,demo->in_buf,demo->in_len,
                        MAX_BUFFER,&demo->in_act,GUI_INPUT_DEFAULT);
    demo->group_off = gui_panel_group_end(panel, &tab);
}

int
main(int argc, char *argv[])
{
    /* Platform */
    long dt;
    long started;
    XWindow xw;
    XSurface *surf;
    XFont *xfont;
    gui_bool running = gui_true;
    struct demo demo;

    /* GUI */
    struct gui_input in;
    struct gui_font font;
    struct gui_memory memory;
    struct gui_memory_status status;
    struct gui_config config;
    struct gui_canvas canvas;
    struct gui_command_buffer buffer;
    struct gui_command_list list;
    struct gui_panel_layout layout;
    struct gui_panel panel;

    /* Window */
    UNUSED(argc); UNUSED(argv);
    memset(&xw, 0, sizeof xw);
    xw.dpy = XOpenDisplay(NULL);
    xw.root = DefaultRootWindow(xw.dpy);
    xw.screen = XDefaultScreen(xw.dpy);
    xw.vis = XDefaultVisual(xw.dpy, xw.screen);
    xw.cmap = XCreateColormap(xw.dpy,xw.root,xw.vis,AllocNone);
    xw.swa.colormap = xw.cmap;
    xw.swa.event_mask =
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPress |
        ButtonReleaseMask | ButtonMotionMask | Button1MotionMask | PointerMotionMask;
    xw.win = XCreateWindow(xw.dpy, xw.root, 0, 0, WIN_WIDTH, WIN_HEIGHT, 0,
        XDefaultDepth(xw.dpy, xw.screen), InputOutput,
        xw.vis, CWEventMask | CWColormap, &xw.swa);
    XStoreName(xw.dpy, xw.win, "X11");
    XMapWindow(xw.dpy, xw.win);
    XGetWindowAttributes(xw.dpy, xw.win, &xw.attr);
    xw.width = (unsigned int)xw.attr.width;
    xw.height = (unsigned int)xw.attr.height;
    surf = surface_create(xw.dpy, xw.screen, xw.win, xw.width, xw.height);
    xfont = font_create(xw.dpy, "fixed");

    /* GUI */
    memset(&in, 0, sizeof in);
    memory.memory = calloc(MAX_MEMORY, 1);
    memory.size = MAX_MEMORY;
    gui_buffer_init_fixed(&buffer, &memory, GUI_CLIP);

    font.userdata = xfont;
    font.height = (gui_float)xfont->height;
    font.width = font_get_text_width;
    gui_default_config(&config);
    gui_panel_init(&panel, 50, 50, 420, 300,
        GUI_PANEL_BORDER|GUI_PANEL_MOVEABLE|
        GUI_PANEL_CLOSEABLE|GUI_PANEL_SCALEABLE|
        GUI_PANEL_MINIMIZABLE, &config, &font);

    /* Demo */
    memset(&demo, 0, sizeof(demo));
    demo.tab_min = gui_true;
    demo.spinner = 100;
    demo.slider = 2.0f;
    demo.prog = 60;

    while (running) {
        /* Input */
        XEvent evt;
        started = timestamp();
        gui_input_begin(&in);
        while (XCheckWindowEvent(xw.dpy, xw.win, xw.swa.event_mask, &evt)) {
            if (evt.type == KeyPress) key(&xw, &in, &evt, gui_true);
            else if (evt.type == KeyRelease) key(&xw, &in, &evt, gui_false);
            else if (evt.type == ButtonPress) btn(&in, &evt, gui_true);
            else if (evt.type == ButtonRelease) btn(&in, &evt, gui_false);
            else if (evt.type == MotionNotify) motion(&in, &evt);
            else if (evt.type == Expose || evt.type == ConfigureNotify)
                resize(&xw, surf);
        }
        gui_input_end(&in);

        /* GUI */
        gui_buffer_begin(&canvas, &buffer, xw.width, xw.height);
        running = gui_panel_begin(&layout, &panel, "Demo", &canvas, &in);
        demo_panel(&layout, &demo);
        gui_panel_end(&layout, &panel);
        gui_buffer_end(&list, &buffer, &canvas, &status);

        /* Draw */
        XClearWindow(xw.dpy, xw.win);
        surface_clear(surf, 0x00646464);
        draw(surf, &list);
        surface_blit(xw.win, surf, xw.width, xw.height);
        XFlush(xw.dpy);

        /* Timing */
        dt = timestamp() - started;
        if (dt < DTIME)
            sleep_for(DTIME - dt);
    }

    free(memory.memory);
    font_del(xw.dpy, xfont);
    surface_del(surf);
    XUnmapWindow(xw.dpy, xw.win);
    XFreeColormap(xw.dpy, xw.cmap);
    XDestroyWindow(xw.dpy, xw.win);
    XCloseDisplay(xw.dpy);
    return 0;
}

