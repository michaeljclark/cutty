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
#include "format.h"
#include "app.h"

#include "ui9.h"
#include "terminal.h"
#include "process.h"
#include "cellgrid.h"
#include "typeface.h"
#include "render.h"

using namespace std::chrono;

/* globals */

static font_manager_ft manager;
static std::unique_ptr<cu_term> term;
static std::unique_ptr<cu_cellgrid> cg;
static std::unique_ptr<cu_render> render;
static std::unique_ptr<cu_process> process;

static bool help_text = false;
static bool overlay_stats = false;
static bool execute_args = false;
static std::string output_file = "capture.png";

static const char* default_path = "bash";
static const char * const default_argv[] = { "-bash", NULL };
static const char* exec_path = default_path;
static const char * const * exec_argv = default_argv;
static std::vector<const char*> exec_vec;

static OSMesaContext ctx;
static uint8_t *buffer;

static void osmesa_init(int width, int height)
{
    /* Create an RGBA-mode context */
    if (!(ctx = OSMesaCreateContextExt( OSMESA_RGBA, 16, 0, 0, NULL))) {
        Panic("OSMesaCreateContext failed!\n");
    }

    /* Allocate the image buffer */
    if (!(buffer = (uint8_t*)malloc(width * height * sizeof(uint)))) {
        Panic("Alloc image buffer failed!\n");
    }

    /* Bind the buffer to the context and make it current */
    if (!OSMesaMakeCurrent( ctx, buffer, GL_UNSIGNED_BYTE, width, height)) {
        Panic("OSMesaMakeCurrent failed!\n");
    }
}

static void flip_buffer_y(uint* buffer, uint width, uint height)
{
    /* Iterate only half the buffer to get a full flip */
    size_t rows = height >> 1;
    size_t line_size = width * sizeof(uint);
    uint* scan_line = (uint*)alloca(line_size);

    for (uint rowIndex = 0; rowIndex < rows; rowIndex++)
    {
        size_t l1 = rowIndex * width;
        size_t l2 = (height - rowIndex - 1) * width;
        memcpy(scan_line, buffer + l1, line_size);
        memcpy(buffer + l1, buffer + l2, line_size);
        memcpy(buffer + l2, scan_line, line_size);
    }
}

static void capture_app(int argc, char **argv)
{
    term = std::unique_ptr<cu_term>(cu_term_new());
    cg = std::unique_ptr<cu_cellgrid>(cu_cellgrid_new(&manager, term.get(), true));
    render = std::unique_ptr<cu_render>(cu_render_new(&manager, cg.get()));
    process = std::unique_ptr<cu_process>(cu_process_new());
    render->set_overlay(overlay_stats);

    cg->flags &= ~cu_cellgrid_background;
    osmesa_init((uint)cg->width, (uint)cg->height);

    render->initialize();
    render->reshape(cg->width, cg->height, 1.f/cg->rscale);

    cu_winsize dim = cu_cellgrid_visible(cg.get());
    cu_term_set_dim(term.get(), dim);
    cu_term_reset(term.get());

    int fd = process->exec(dim, exec_path, exec_argv);
    cu_term_set_fd(term.get(), fd);

    uint running = 1;
    while (running) {
        process->winsize(render->update());
        render->display();
        glFlush();
        if (term->needs_capture) {
            flip_buffer_y((uint*)buffer, (uint)cg->width, (uint)cg->height);
            image::saveToFile(output_file, image::createBitmap
                ((uint)cg->width, (uint)cg->height, pixel_format_rgba, buffer));
            break;
        }
        do if (cu_term_io(term.get()) < 0) {
            running = 0;
        }
        while (cu_term_process(term.get()) > 0);
    }

    cu_term_close(term.get());

    OSMesaDestroyContext(ctx);
    free(buffer);
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
        "  -o, --output              capture output filename\n"
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
        } else if (match_opt(argv[i], "-o", "--output")) {
            if (check_param(++i == argc, "--output")) break;
            output_file = argv[i++];
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
    capture_app(argc, argv);

    return 0;
}

declare_main(app_main)
