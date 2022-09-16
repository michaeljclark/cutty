#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>

#include <functional>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <chrono>

#include "binpack.h"
#include "image.h"
#include "color.h"
#include "utf8.h"
#include "draw.h"
#include "font.h"
#include "glyph.h"
#include "canvas.h"
#include "color.h"
#include "logger.h"
#include "file.h"
#include "app.h"
#include "ui9.h"

#include "timestamp.h"
#include "teletype.h"
#include "translate.h"
#include "process.h"
#include "cellgrid.h"
#include "render.h"

using namespace std::chrono;

#if defined(__APPLE__)
bool resource_prefix = false;
#else
bool resource_prefix = true;
#endif

/* globals */

static font_manager_ft manager;
static std::unique_ptr<tty_teletype> tty;
static std::unique_ptr<tty_cellgrid> cg;
static std::unique_ptr<tty_render> render;
static std::unique_ptr<tty_process> process;
static GLFWwindow* window;
static vec2 mouse_pos;

static bool help_text = false;
static bool overlay_stats = false;
static bool execute_args = false;
static bool enable_linenumbers = false;
static bool enable_timestamps = false;
static bool enable_scrollbars = false;

static const char* app_name = "cutty";
static const char* default_path = "bash";
static const char * const default_argv[] = { "-bash", NULL };
static const char* exec_path = default_path;
static const char * const * exec_argv = default_argv;
static std::vector<const char*> exec_vec;


/* cursors */

static int cursor_names[2] = { GLFW_ARROW_CURSOR, GLFW_IBEAM_CURSOR };
static GLFWcursor *cursor_objects[2];

static void init_cursors()
{
    for (size_t i = 0; i < array_size(cursor_names); i++) {
        cursor_objects[i] = glfwCreateStandardCursor(cursor_names[i]);
    }
}

static void init_keymap()
{
    std::vector<char> input_map;
    if (resource_prefix) {
        input_map = load_file("Resources/settings/input.map");
    } else {
        input_map = load_file("settings/input.map");
    }
    tty_keymap_init(input_map);
}

void app_set_cursor(app_cursor cursor)
{
    glfwSetCursor(window, cursor_objects[cursor]);
}

const char* app_get_clipboard()
{
    return glfwGetClipboardString(window);
}

void app_set_clipboard(const char* str)
{
    glfwSetClipboardString(window, str);
}

/* keyboard callback */

static void keyboard(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (tty->keyboard(key, scancode, action, mods)) {
        if (tty->scroll_row() != 0) {
            tty->set_scroll_row(0);
        }
    }
}

/* mouse callbacks */

static void scroll(GLFWwindow* window, double xoffset, double yoffset)
{
    //
}

static char q;
static char b;

static bool mouse_button_ui9(int button, int action, int mods, vec3 pos)
{
    switch(button) {
    case GLFW_MOUSE_BUTTON_LEFT:  b = ui9::left_button;  break;
    case GLFW_MOUSE_BUTTON_RIGHT: b = ui9::right_button; break;
    }
    switch(action) {
    case GLFW_PRESS:   q = ui9::pressed;  break;
    case GLFW_RELEASE: q = ui9::released; break;
    }
    vec3 v = cg->get_canvas()->get_inverse_transform() * pos;
    ui9::MouseEvent evt{{ui9::mouse, q}, b, v};
    bool handled = cg->get_root()->dispatch(&evt.header);
    return (!handled) ? cg->mouse_event(&evt) : false;
}

static bool mouse_motion_ui9(vec3 pos)
{
    vec3 v = cg->get_canvas()->get_inverse_transform() * pos;
    ui9::MouseEvent evt{{ui9::mouse, ui9::motion}, b, v};
    bool handled = false;
    handled |= cg->get_root()->dispatch(&evt.header);
    handled |= cg->mouse_event(&evt);
    return handled;
}

static bool scroll_wheel_ui9(vec3 v)
{
    ui9::MouseEvent evt{{ui9::mouse, ui9::wheel}, b, v};
    bool handled = false;
    handled |= cg->get_root()->dispatch(&evt.header);
    handled |= cg->mouse_event(&evt);
    return handled;
}

static void scroll_wheel(GLFWwindow* window, double xoffset, double yoffset)
{
    if (scroll_wheel_ui9(vec3(xoffset, yoffset, 0.f))) {
        tty->set_needs_update();
    }
}

static void mouse_button(GLFWwindow* window, int button, int action, int mods)
{
    if (mouse_button_ui9(button, action, mods, vec3(mouse_pos, 1))) {
        tty->set_needs_update();
    }
}

static void cursor_position(GLFWwindow* window, double xpos, double ypos)
{
    mouse_pos = vec2(xpos, ypos);

    if (mouse_motion_ui9(vec3(mouse_pos, 1))) {
        tty->set_needs_update();
        return;
    }
}

static void cursor_enter(GLFWwindow* window, int entered)
{
    app_set_cursor(entered ? app_cursor_ibeam : app_cursor_arrow);
}

static void reshape()
{
    int framebuffer_width, framebuffer_height;
    int window_width, window_height;

    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    glViewport(0, 0, framebuffer_width, framebuffer_height);

    float scale = sqrtf((float)(framebuffer_width * framebuffer_height) /
                       (float)(window_width * window_height));

    render->reshape(window_width, window_height);

    tty_style lstyle = cg->get_style();
    tty_style style = lstyle;
    style.width = (float)window_width;
    style.height = (float)window_height;
    style.rscale = 1.0f/scale;
    if (style != lstyle) {
        cg->set_style(style);
    }

    tty_winsize dim = cg->get_winsize();
    tty_winsize ldim = tty->get_winsize();
    if (dim != ldim) {
        tty->set_winsize(dim);
        process->winsize(dim);
    }
}

static void framebuffer_size(GLFWwindow* window, int w, int h)
{
    tty->set_needs_update();
    reshape();
    render->update();
    render->display();
    glfwSwapBuffers(window);
}

static void window_focus(GLFWwindow* window, int focused)
{
    tty->set_needs_update();
    cg->set_flag(tty_cellgrid_focused, focused);
    render->update();
    render->display();
    glfwSwapBuffers(window);
}

static void window_refresh(GLFWwindow* window)
{
    render->display();
    glfwSwapBuffers(window);
}

static void window_pos(GLFWwindow* window, int x, int y)
{
    //printf("%s: x=%d y=%d\n", __func__, x, y);
}

static void window_size(GLFWwindow* window, int w, int h)
{
    //printf("%s: w=%d h=%d\n", __func__, w, h);
}

static void tty_app(int argc, char **argv)
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, CTX_OPENGL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, CTX_OPENGL_MINOR);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);

    /*
     * we need to scan font directory for caching to work, as it uses
     * font ids assigned during scanning. this also means that if the
     * font directory has changed, then cached font ids will be wrong
     */
    if (manager.msdf_enabled) {
        manager.scanFontDir("fonts");
    }

    tty = std::unique_ptr<tty_teletype>(tty_new());
    cg = std::unique_ptr<tty_cellgrid>(tty_cellgrid_new(&manager, tty.get()));
    render = std::unique_ptr<tty_render>(tty_render_new(&manager, cg.get()));
    process = std::unique_ptr<tty_process>(tty_process_new());
    render->set_overlay(overlay_stats);

    if (enable_timestamps) {
        cg->set_flag(tty_cellgrid_timestamps, true);
    }
    if (enable_linenumbers) {
        cg->set_flag(tty_cellgrid_linenumbers, true);
    }
    if (enable_scrollbars) {
        cg->set_flag(tty_cellgrid_scrollbars, true);
    }

    tty_style style = cg->get_style();

    window = glfwCreateWindow((int)style.width, (int)style.height, app_name, NULL, NULL);
    glfwMakeContextCurrent(window);
    gladLoadGL();
    glfwSwapInterval(1);
    glfwSetScrollCallback(window, scroll);
    glfwSetKeyCallback(window, keyboard);
    glfwSetMouseButtonCallback(window, mouse_button);
    glfwSetScrollCallback(window, scroll_wheel);
    glfwSetCursorEnterCallback(window, cursor_enter);
    glfwSetCursorPosCallback(window, cursor_position);
    glfwSetFramebufferSizeCallback(window, framebuffer_size);
    glfwSetWindowFocusCallback(window, window_focus);
    glfwSetWindowRefreshCallback(window, window_refresh);
    glfwSetWindowPosCallback(window, window_pos);
    glfwSetWindowSizeCallback(window, window_size);

    int framebuffer_width, framebuffer_height;
    int window_width, window_height;
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    float scale = sqrtf((float)(framebuffer_width * framebuffer_height) /
                       (float)(window_width * window_height));

    init_keymap();
    init_cursors();
    render->initialize();

    reshape();
    tty_winsize dim = cg->get_winsize();
    tty->set_winsize(dim);
    tty->reset();

    int fd = process->exec(dim, exec_path, exec_argv, true /* fixme */);
    tty->set_fd(fd);

    while (!glfwWindowShouldClose(window)) {
        render->update();
        render->display();
        glfwSwapBuffers(window);
        glfwPollEvents();
        do if (tty->io() < 0) {
            glfwSetWindowShouldClose(window, 1);
        }
        while (tty->proc() > 0);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    tty->close();
}

/* help text */

void print_help(int argc, char **argv)
{
    fprintf(stderr,
        "Usage: %s [options] [args]\n"
        "  -h, --help                command line help\n"
        "  -t, --trace               log trace messages\n"
        "  -d, --debug               log debug messages\n"
        "  -x, --execute             execute remaining args\n"
        "  -S, --scroll-bars         enable scroll bars\n"
        "  -L, --line-numbers        enable line numbers column\n"
        "  -T, --time-stamps         enable time stamps column\n"
        "  -y, --overlay-stats       show statistics overlay\n"
        "  -m, --enable-msdf         enable MSDF font rendering\n",
        argv[0]);
}

/* option parsing */

bool check_param(bool cond, const char *param)
{
    if (cond) {
        printf("error: %s requires parameter\n", param);
    }
    return (help_text = cond);
}

bool match_opt(const char *arg, const char *opt, const char *longopt)
{
    return strcmp(arg, opt) == 0 || strcmp(arg, longopt) == 0;
}

void parse_options(int argc, char **argv)
{
    int i = 1;
    while (i < argc) {
        if (match_opt(argv[i], "-h", "--help")) {
            help_text = true;
            i++;
        } else if (match_opt(argv[i], "-t", "--trace")) {
            logger::set_level(logger::L::Ltrace);
            i++;
        } else if (match_opt(argv[i], "-d", "--debug")) {
            logger::set_level(logger::L::Ldebug);
            i++;
        } else if (match_opt(argv[i], "-x", "--execute")) {
            execute_args = true;
            i++;
        } else if (match_opt(argv[i], "-y", "--overlay-stats")) {
            overlay_stats = true;
            i++;
        } else if (match_opt(argv[i], "-L", "--line-numbers")) {
            enable_linenumbers = true;
            i++;
        } else if (match_opt(argv[i], "-T", "--time-stamps")) {
            enable_timestamps = true;
            i++;
        } else if (match_opt(argv[i], "-S", "--scroll-bars")) {
            enable_scrollbars = true;
            i++;
        } else if (match_opt(argv[i], "-m", "--enable-msdf")) {
            manager.msdf_enabled = true;
            manager.msdf_autoload = true;
            i++;
        } else {
            if (!execute_args) {
                fprintf(stderr, "error: unknown option: %s\n", argv[i]);
                help_text = true;
            }
            break;
        }
    }

    if (execute_args) {
        if (argc - i < 1) {
            fprintf(stderr, "error: -x requires args\n");
            exit(1);
        }
        while (i < argc) {
            exec_vec.push_back(argv[i++]);
        }
        exec_vec.push_back(NULL);
        exec_path = exec_vec[0];
        exec_argv = exec_vec.data();
    }

    if (help_text) {
        print_help(argc, argv);
        exit(1);
    }

}

/* entry point */

static int app_main(int argc, char** argv)
{
    parse_options(argc, argv);
    tty_app(argc, argv);

    return 0;
}

declare_main(app_main)
