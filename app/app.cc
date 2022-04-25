#include <cstdio>
#include <cstring>
#include <cerrno>

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
#include "ui9.h"
#include "app.h"

#include "terminal.h"
#include "process.h"
#include "cellgrid.h"
#include "render.h"
//#include "ui.h"

using namespace std::chrono;

/* globals */

static font_manager_ft manager;
static std::unique_ptr<cu_term> term;
static std::unique_ptr<cu_cellgrid> cg;
static std::unique_ptr<cu_render> render;
static std::unique_ptr<cu_process> process;
static GLFWwindow* window;
static vec2 mouse_pos;

static bool help_text = false;
static bool overlay_stats = false;
static bool execute_args = false;

static const char* default_path = "bash";
static const char * const default_argv[] = { "-bash", NULL };
static const char* exec_path = default_path;
static const char * const * exec_argv = default_argv;
static std::vector<const char*> exec_vec;


/* keyboard callback */

static void keyboard(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    cu_term_keyboard(term.get(), key, scancode, action, mods);
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
    return cg->get_root()->dispatch(&evt.header);
}

static void mouse_button(GLFWwindow* window, int button, int action, int mods)
{
    if (mouse_button_ui9(button, action, mods, vec3(mouse_pos, 1))) {
        return;
    }
}

static bool mouse_motion_ui9(vec3 pos)
{
    vec3 v = cg->get_canvas()->get_inverse_transform() * pos;
    ui9::MouseEvent evt{{ui9::mouse, ui9::motion}, b, v};
    return cg->get_root()->dispatch(&evt.header);
}

static void cursor_position(GLFWwindow* window, double xpos, double ypos)
{
    mouse_pos = vec2(xpos, ypos);

    if (mouse_motion_ui9(vec3(mouse_pos, 1))) {
        term->needs_update = 1;
        return;
    }
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

    render->reshape(window_width, window_height, scale);
}

static void framebuffer_size(GLFWwindow* window, int w, int h)
{
    term->needs_update = 1;
    reshape();
    cu_winsize dim = render->update();
    if (process->winsize(dim)) {
        cu_term_set_dim(term.get(), dim);
    }
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

static void cu_term_app(int argc, char **argv)
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, CTX_OPENGL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, CTX_OPENGL_MINOR);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);

    term = std::unique_ptr<cu_term>(cu_term_new());
    cg = std::unique_ptr<cu_cellgrid>(cu_cellgrid_new(&manager, term.get()));
    render = std::unique_ptr<cu_render>(cu_render_new(&manager, cg.get()));
    process = std::unique_ptr<cu_process>(cu_process_new());
    render->set_overlay(overlay_stats);

    window = glfwCreateWindow((int)cg->width, (int)cg->height, argv[0], NULL, NULL);
    glfwMakeContextCurrent(window);
    gladLoadGL();
    glfwSwapInterval(1);
    glfwSetScrollCallback(window, scroll);
    glfwSetKeyCallback(window, keyboard);
    glfwSetMouseButtonCallback(window, mouse_button);
    glfwSetCursorPosCallback(window, cursor_position);
    glfwSetFramebufferSizeCallback(window, framebuffer_size);
    glfwSetWindowRefreshCallback(window, window_refresh);
    glfwSetWindowPosCallback(window, window_pos);
    glfwSetWindowSizeCallback(window, window_size);

    render->initialize();
    reshape();

    cu_winsize dim = cg->visible();
    cu_term_set_dim(term.get(), dim);
    cu_term_reset(term.get());

    int fd = process->exec(dim, exec_path, exec_argv);
    cu_term_set_fd(term.get(), fd);

    while (!glfwWindowShouldClose(window)) {
        cu_winsize dim = render->update();
        if (process->winsize(dim)) {
            cu_term_set_dim(term.get(), dim);
        }
        render->display();
        glfwSwapBuffers(window);
        glfwPollEvents();
        do if (cu_term_io(term.get()) < 0) {
            glfwSetWindowShouldClose(window, 1);
        }
        while (cu_term_process(term.get()) > 0);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    cu_term_close(term.get());
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
    cu_term_app(argc, argv);

    return 0;
}

declare_main(app_main)
