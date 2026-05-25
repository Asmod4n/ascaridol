/*
 * tools/ascaridol/ascaridol.cc — Ascaridol runtime: main(), Ascaridol.run,
 *                                Ascaridol.ready, the platform-specific
 *                                work wrapping the live webview, and the
 *                                HTML-driven native menu installer.
 *
 * Architecture recap:
 *
 *   src/ascaridol_methods.cc defines Ascaridol.title=, .html=, .bind,
 *   .dispatch, .add_native_event, etc. Each method dispatches off-main
 *   calls onto main via webview::dispatch, capturing CBOR-serialized
 *   payloads as plain std::strings into the dispatch lambdas. On main,
 *   the dispatched lambda calls into one of the ascaridol_*_on_main
 *   helpers defined here.
 *
 *   This file:
 *     - Implements the on_main helpers, which run with full access to
 *       the live webview and main's mrb_state.
 *     - Defines Ascaridol.run (kwarg-driven setup, yields the Ascaridol
 *       module to a block, runs ready hook, then webview::run blocks).
 *     - Defines Ascaridol.ready (one-shot post-setup hook).
 *     - Defines Ascaridol.enable_html_menu — opt-in installer that wires
 *       a bootstrap JS scraper to the native menu code for the current
 *       backend (Win32 / Cocoa / GTK3 / GTK4).
 *     - Owns the C main() that opens mrb, exposes ARGV, runs the
 *       embedded user script, cleans up.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  define _WINSOCK_DEPRECATED_NO_WARNINGS
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <commctrl.h>
#  pragma comment(lib, "Comctl32")
#endif

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/numeric.h>
#include <mruby/chrono.h>
#include <mruby/presym.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <mruby/compile.h>
#include <mruby/irep.h>
#include <mruby/io.h>

#include <mruby/cpp_helpers.hpp>
#include <mruby/num_helpers.hpp>
#include <mruby/fast_json.h>

#include <webview/webview.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../../src/webview_internal.h"

/* ========================================================================= */
/* Lifecycle state.                                                          */
/* ========================================================================= */

static mrb_value g_ready_hook;
static bool      g_ready_hook_set = false;
static bool      g_ready_fired    = false;


/* ========================================================================= */
/* Owner CDATA and main-state gate                                           */
/* ========================================================================= */

MRB_CPP_DEFINE_TYPE(webview::webview, Webview)

struct AscaridolWVOwner {
    webview::webview* wv = nullptr;
    ~AscaridolWVOwner() {
        if (wv) {
            g_wv.store(nullptr, std::memory_order_release);
            delete wv;
            wv = nullptr;
        }
    }
};

MRB_CPP_DEFINE_TYPE(AscaridolWVOwner, AscaridolWVOwner)

static void
ascaridol_require_main_state(mrb_state* mrb, const char* method_name)
{
    if (!ascaridol_in_main_state(mrb)) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "%s can only be called from the main thread mruby vm; "
            "use Ascaridol.dispatch to run code on main from a worker",
            method_name);
    }
}


/* ========================================================================= */
/* Bind machinery                                                            */
/* ========================================================================= */

static mrb_value
bindings_hash(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value h = mrb_iv_get(mrb, asc, MRB_SYM(bindings));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, asc, MRB_SYM(bindings), h);
    }
    return h;
}

static mrb_value
async_bindings_hash(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value h = mrb_iv_get(mrb, asc, MRB_SYM(async_bindings));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, asc, MRB_SYM(async_bindings), h);
    }
    return h;
}

struct bind_step {
    mrb_value src;
    mrb_value proc;
    mrb_value parsed;
    mrb_value result;
};

static mrb_value
bind_parse_body(mrb_state* mrb, void* p)
{
    auto* s = static_cast<bind_step*>(p);
    struct RClass* json = mrb_module_get_id(mrb, MRB_SYM(JSON));
    return mrb_funcall_id(mrb, mrb_obj_value(json), MRB_SYM(parse), 1, s->src);
}

static mrb_value
bind_invoke_body(mrb_state* mrb, void* p)
{
    auto* s = static_cast<bind_step*>(p);
    mrb_int argc = RARRAY_LEN(s->parsed);
    mrb_value* argv = RARRAY_PTR(s->parsed);
    return mrb_yield_argv(mrb, s->proc, argc, argv);
}

static mrb_value
bind_dump_body(mrb_state* mrb, void* p)
{
    auto* s = static_cast<bind_step*>(p);
    return mrb_json_dump(mrb, s->result);
}

static std::string
to_std_string(mrb_value v)
{
    return std::string{ RSTRING_PTR(v), static_cast<size_t>(RSTRING_LEN(v)) };
}

static mrb_value
str_from(mrb_state* mrb, const std::string& s)
{
    return mrb_str_new(mrb, s.data(), s.size());
}

static std::string
make_error_json_str(mrb_state* mrb, mrb_value name, mrb_value message,
    mrb_value backtrace)
{
    mrb_value h = mrb_hash_new_capa(mrb, 3);
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(name)), name);
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(message)), message);
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(backtrace)),
        mrb_array_p(backtrace) ? backtrace : mrb_ary_new(mrb));
    return to_std_string(mrb_json_dump(mrb, h));
}

static void
invoke_bound_proc(mrb_state* mrb, mrb_sym name_sym, webview::webview* wv,
    const std::string& id, const std::string& req)
{
    mrb_int ai = mrb_gc_arena_save(mrb);

    mrb_value proc = mrb_hash_fetch(mrb, bindings_hash(mrb),
        mrb_symbol_value(name_sym),
        mrb_undef_value());
    if (!mrb_proc_p(proc)) {
        wv->resolve(id, 1,
            "{\"name\":\"Error\",\"message\":\"binding not registered\"}");
        mrb_gc_arena_restore(mrb, ai);
        return;
    }

    bind_step step;
    step.src = str_from(mrb, req);
    step.proc = proc;
    step.parsed = mrb_nil_value();
    step.result = mrb_nil_value();
    mrb_bool err = FALSE;

    mrb_value parsed = mrb_protect_error(mrb, bind_parse_body, &step, &err);
    if (err) {
        mrb_value message = mrb_funcall_id(mrb, parsed, MRB_SYM(message), 0);
        mrb_value backtrace = mrb_funcall_id(mrb, parsed, MRB_SYM(backtrace), 0);
        wv->resolve(id, 1,
            make_error_json_str(mrb, mrb_str_new_lit(mrb, "ParseError"),
                message, backtrace));
        mrb_gc_arena_restore(mrb, ai);
        return;
    }
    if (!mrb_array_p(parsed)) {
        mrb_value tmp = mrb_ary_new_capa(mrb, 1);
        mrb_ary_push(mrb, tmp, parsed);
        parsed = tmp;
    }
    step.parsed = parsed;

    err = FALSE;
    mrb_value result = mrb_protect_error(mrb, bind_invoke_body, &step, &err);
    if (err) {
        mrb_value message = mrb_funcall_id(mrb, result, MRB_SYM(message), 0);
        mrb_value cls = mrb_funcall_id(mrb, result, MRB_SYM(class), 0);
        mrb_value name = mrb_funcall_id(mrb, cls, MRB_SYM(name), 0);
        mrb_value backtrace = mrb_funcall_id(mrb, result, MRB_SYM(backtrace), 0);
        if (!mrb_string_p(name)) name = mrb_str_new_lit(mrb, "Error");
        wv->resolve(id, 1, make_error_json_str(mrb, name, message, backtrace));
        mrb_gc_arena_restore(mrb, ai);
        return;
    }
    step.result = result;

    err = FALSE;
    mrb_value json_result = mrb_protect_error(mrb, bind_dump_body, &step, &err);
    std::string out = err ? std::string{ "null" } : to_std_string(json_result);
    wv->resolve(id, 0, out);
    mrb_gc_arena_restore(mrb, ai);
}

static void
invoke_bound_proc_async(mrb_state* mrb, mrb_sym name_sym, webview::webview* wv,
    const std::string& id, const std::string& req)
{
    mrb_int ai = mrb_gc_arena_save(mrb);

    mrb_value proc = mrb_hash_fetch(mrb, async_bindings_hash(mrb),
        mrb_symbol_value(name_sym),
        mrb_undef_value());
    if (!mrb_proc_p(proc)) {
        wv->resolve(id, 1,
            "{\"name\":\"Error\",\"message\":\"binding not registered\"}");
        mrb_gc_arena_restore(mrb, ai);
        return;
    }

    bind_step step;
    step.src = str_from(mrb, req);
    step.proc = proc;
    step.parsed = mrb_nil_value();
    step.result = mrb_nil_value();
    mrb_bool err = FALSE;

    mrb_value parsed = mrb_protect_error(mrb, bind_parse_body, &step, &err);
    if (err) {
        mrb_value message = mrb_funcall_id(mrb, parsed, MRB_SYM(message), 0);
        mrb_value backtrace = mrb_funcall_id(mrb, parsed, MRB_SYM(backtrace), 0);
        wv->resolve(id, 1,
            make_error_json_str(mrb, mrb_str_new_lit(mrb, "ParseError"),
                message, backtrace));
        mrb_gc_arena_restore(mrb, ai);
        return;
    }
    if (!mrb_array_p(parsed)) {
        mrb_value tmp = mrb_ary_new_capa(mrb, 1);
        mrb_ary_push(mrb, tmp, parsed);
        parsed = tmp;
    }

    mrb_value id_str = mrb_str_new(mrb, id.data(), id.size());
    mrb_value args_with_id = mrb_ary_new_capa(mrb, RARRAY_LEN(parsed) + 1);
    mrb_ary_push(mrb, args_with_id, id_str);
    for (mrb_int i = 0; i < RARRAY_LEN(parsed); i++) {
        mrb_ary_push(mrb, args_with_id, mrb_ary_ref(mrb, parsed, i));
    }
    step.parsed = args_with_id;

    err = FALSE;
    mrb_value result = mrb_protect_error(mrb, bind_invoke_body, &step, &err);
    if (err) {
        mrb_value message = mrb_funcall_id(mrb, result, MRB_SYM(message), 0);
        mrb_value cls = mrb_funcall_id(mrb, result, MRB_SYM(class), 0);
        mrb_value name = mrb_funcall_id(mrb, cls, MRB_SYM(name), 0);
        mrb_value backtrace = mrb_funcall_id(mrb, result, MRB_SYM(backtrace), 0);
        if (!mrb_string_p(name)) name = mrb_str_new_lit(mrb, "Error");
        wv->resolve(id, 1, make_error_json_str(mrb, name, message, backtrace));
        mrb_gc_arena_restore(mrb, ai);
        return;
    }
    /* No resolve on success — user's block calls Ascaridol.resolve. */
    mrb_gc_arena_restore(mrb, ai);
}

void
ascaridol_bind_on_main(mrb_state* mrb, webview::webview* wv,
    mrb_sym name_sym, const std::string& name, mrb_value proc)
{
    mrb_value bh = bindings_hash(mrb);

    if (mrb_proc_p(mrb_hash_fetch(mrb, bh, mrb_symbol_value(name_sym), mrb_undef_value()))) {
        mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), proc);
        return;
    }

    auto err = wv->bind(name,
        [mrb, name_sym, wv](std::string id, std::string req, void*) {
            invoke_bound_proc(mrb, name_sym, wv, id, req);
        },
        nullptr);
    ascaridol_check_result(mrb, err);

    mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), proc);
}

void
ascaridol_bind_async_on_main(mrb_state* mrb, webview::webview* wv,
    mrb_sym name_sym, const std::string& name, mrb_value proc)
{
    mrb_value bh = async_bindings_hash(mrb);

    if (mrb_proc_p(mrb_hash_fetch(mrb, bh, mrb_symbol_value(name_sym), mrb_undef_value()))) {
        mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), proc);
        return;
    }

    auto err = wv->bind(name,
        [mrb, name_sym, wv](std::string id, std::string req, void*) {
            invoke_bound_proc_async(mrb, name_sym, wv, id, req);
        },
        nullptr);
    ascaridol_check_result(mrb, err);

    mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), proc);
}

void
ascaridol_unbind_on_main(mrb_state* mrb, webview::webview* wv, mrb_sym name_sym,
    const std::string& name)
{
    mrb_value sym_v = mrb_symbol_value(name_sym);
    mrb_value sync_h = bindings_hash(mrb);
    mrb_value async_h = async_bindings_hash(mrb);
    bool in_sync = mrb_proc_p(mrb_hash_fetch(mrb, sync_h, sym_v, mrb_undef_value()));
    bool in_async = mrb_proc_p(mrb_hash_fetch(mrb, async_h, sym_v, mrb_undef_value()));

    auto err = wv->unbind(name);
    ascaridol_check_result(mrb, err);

    if (in_sync)  mrb_hash_delete_key(mrb, sync_h, sym_v);
    if (in_async) mrb_hash_delete_key(mrb, async_h, sym_v);
}

mrb_value
ascaridol_bindings_on_main(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value sync_h = mrb_iv_get(mrb, asc, MRB_SYM(bindings));
    mrb_value async_h = mrb_iv_get(mrb, asc, MRB_SYM(async_bindings));

    mrb_value result = mrb_ary_new(mrb);
    if (mrb_hash_p(sync_h)) {
        mrb_value keys = mrb_hash_keys(mrb, sync_h);
        for (mrb_int i = 0; i < RARRAY_LEN(keys); i++) {
            mrb_ary_push(mrb, result, mrb_ary_ref(mrb, keys, i));
        }
    }
    if (mrb_hash_p(async_h)) {
        mrb_value keys = mrb_hash_keys(mrb, async_h);
        for (mrb_int i = 0; i < RARRAY_LEN(keys); i++) {
            mrb_ary_push(mrb, result, mrb_ary_ref(mrb, keys, i));
        }
    }
    return result;
}

mrb_value
ascaridol_native_handle_on_main(mrb_state* mrb, webview::webview* wv,
    mrb_value kind_v)
{
    if (!mrb_symbol_p(kind_v)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "handle kind must be a Symbol");
    }
    mrb_sym s = mrb_symbol(kind_v);

    void* p = nullptr;
    if (s == MRB_SYM(window)) {
        auto r = wv->window();
        if (r.ok()) p = r.value();
    }
    else if (s == MRB_SYM(widget)) {
        auto r = wv->widget();
        if (r.ok()) p = r.value();
    }
    else if (s == MRB_SYM(browser_controller)) {
        auto r = wv->browser_controller();
        if (r.ok()) p = r.value();
    }
    else {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown handle kind: %v", kind_v);
    }
    return p ? mrb_cptr_value(mrb, p) : mrb_nil_value();
}

/* ========================================================================= */
/* add_native_event / remove_native_event                                    */
/* ========================================================================= */

static mrb_value
fds_hash(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value h = mrb_iv_get(mrb, asc, MRB_SYM(fds_procs));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, asc, MRB_SYM(fds_procs), h);
    }
    return h;
}

/* ---- Persistent GC root for live Ascaridol::Timer objects --------------
 *
 * The OS owns the timer source (g_timeout, CFRunLoopTimer, SetTimer) which
 * holds a raw pointer to our ud struct. The ud lives inside the Ruby
 * Timer object's CDATA. If nothing in Ruby references the Timer, GC
 * collects it, the destructor frees ud, and the next OS-level callback
 * uses freed memory.
 *
 * timer_map pins every live Timer until cancel / user block returns
 * stop. Key is the native platform timer id (cptr for pointer-shaped
 * ids, uint for guint). Callbacks fetch the Timer back out by the id
 * they already received from the OS. */
static mrb_value
timer_map(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value h = mrb_iv_get(mrb, asc, MRB_SYM(_timer_map));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, asc, MRB_SYM(_timer_map), h);
    }
    return h;
}


enum ascaridol_fd_cond {
    ASCARIDOL_FD_READ  = 1,
    ASCARIDOL_FD_WRITE = 4,
    ASCARIDOL_FD_ERROR = 8,
};

static unsigned int
ascaridol_parse_readiness(mrb_state* mrb, mrb_value v)
{
    if (mrb_nil_p(v) || mrb_undef_p(v)) return ASCARIDOL_FD_READ;
    if (!mrb_symbol_p(v)) {
        mrb_raise(mrb, E_TYPE_ERROR,
            "readiness must be a Symbol (:in, :out, :inout)");
    }
    mrb_sym s = mrb_symbol(v);
    if (s == MRB_SYM(in))    return ASCARIDOL_FD_READ;
    if (s == MRB_SYM(out))   return ASCARIDOL_FD_WRITE;
    if (s == MRB_SYM(inout)) return ASCARIDOL_FD_READ | ASCARIDOL_FD_WRITE;
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
        "invalid readiness %n (want :in, :out, or :inout)", s);
    return 0;
}

static mrb_sym
ascaridol_cond_to_sym(unsigned int conds)
{
    if (conds & ASCARIDOL_FD_ERROR) return MRB_SYM(err);
    bool r = (conds & ASCARIDOL_FD_READ)  != 0;
    bool w = (conds & ASCARIDOL_FD_WRITE) != 0;
    if (r && w) return MRB_SYM(inout);
    if (w)      return MRB_SYM(out);
    return MRB_SYM(in);
}

static mrb_value
mrb_ascaridol_watcher_io(mrb_state* mrb, mrb_value self)
{
    return mrb_iv_get(mrb, self, MRB_SYM(fd));
}

#ifdef WEBVIEW_GTK
#include <glib-unix.h>

struct mrb_ascaridol_fd_ud {
    mrb_state* mrb;
    mrb_value fd;
    mrb_value blk;
    guint id = 0;

    ~mrb_ascaridol_fd_ud() {
        if (id) g_source_remove(id);
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_fd_ud, Ascaridol_Watcher);

static gboolean
on_fd_ready(gint /*fd*/, GIOCondition condition, gpointer user_data)
{
    auto* ud = static_cast<mrb_ascaridol_fd_ud*>(user_data);
    mrb_state* mrb = ud->mrb;
    mrb_int ai = mrb_gc_arena_save(mrb);

    unsigned int conds = 0;
    if (condition & (G_IO_IN | G_IO_HUP))   conds |= ASCARIDOL_FD_READ;
    if (condition & G_IO_OUT)               conds |= ASCARIDOL_FD_WRITE;
    if (condition & (G_IO_ERR | G_IO_NVAL)) conds |= ASCARIDOL_FD_ERROR;

    const mrb_value argv[] = {
        ud->fd, mrb_symbol_value(ascaridol_cond_to_sym(conds))
    };
    mrb_bool cont = mrb_test(mrb_yield_argv(mrb, ud->blk, 2, argv));
    if (!cont) {
        ud->id = 0;
        mrb_hash_delete_key(mrb, fds_hash(mrb), ud->fd);
    }
    mrb_gc_arena_restore(mrb, ai);
    return (gboolean)cont;
}

static mrb_value
ascaridol_fdud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value fd, blk;
    mrb_get_args(mrb, "oo", &fd, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(fd), fd);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_ascaridol_fd_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    ud->mrb = mrb;
    ud->fd = fd;
    ud->blk = blk;
    return self;
}

mrb_value
ascaridol_add_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj, mrb_value blk, unsigned int mask)
{
    int fd = (int)mrb_integer(mrb_type_convert(mrb, fd_obj,
        MRB_TT_INTEGER, MRB_SYM(fileno)));

    struct RClass* asc = mrb_module_get_id(mrb, MRB_SYM(Ascaridol));
    struct RClass* watcher_cls = mrb_class_get_under_id(mrb, asc, MRB_SYM(Watcher));

    mrb_value argv[] = { fd_obj, blk };
    mrb_value ud_obj = mrb_obj_new(mrb, watcher_cls, 2, argv);
    mrb_hash_set(mrb, fds_hash(mrb), fd_obj, ud_obj);

    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, ud_obj);
    GIOCondition cond = (GIOCondition)0;
    if (mask & ASCARIDOL_FD_READ)
        cond = (GIOCondition)(cond | G_IO_IN  | G_IO_HUP | G_IO_ERR);
    if (mask & ASCARIDOL_FD_WRITE)
        cond = (GIOCondition)(cond | G_IO_OUT | G_IO_ERR);

    ud->id = g_unix_fd_add(fd, cond, on_fd_ready, DATA_PTR(ud_obj));
    return ud_obj;
}

static mrb_value
mrb_ascaridol_watcher_update(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    if (ud->id == 0) mrb_raise(mrb, E_IO_ERROR, "watcher already removed");

    mrb_value readiness;
    mrb_get_args(mrb, "o", &readiness);
    unsigned int mask = ascaridol_parse_readiness(mrb, readiness);

    g_source_remove(ud->id);
    int fd = (int)mrb_integer(mrb_type_convert(mrb, ud->fd,
        MRB_TT_INTEGER, MRB_SYM(fileno)));
    GIOCondition cond = (GIOCondition)0;
    if (mask & ASCARIDOL_FD_READ)
        cond = (GIOCondition)(cond | G_IO_IN  | G_IO_HUP | G_IO_ERR);
    if (mask & ASCARIDOL_FD_WRITE)
        cond = (GIOCondition)(cond | G_IO_OUT | G_IO_ERR);
    ud->id = g_unix_fd_add(fd, cond, on_fd_ready, ud);
    return self;
}

static mrb_value
mrb_ascaridol_watcher_remove(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    if (ud->id == 0) mrb_raise(mrb, E_IO_ERROR, "watcher already removed");
    g_source_remove(ud->id);
    ud->id = 0;
    mrb_hash_delete_key(mrb, fds_hash(mrb), ud->fd);
    return mrb_nil_value();
}

void
ascaridol_remove_native_event_on_main(mrb_state* mrb, webview::webview*,
    mrb_value fd_obj)
{
    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (mrb_undef_p(ud_obj)) return;
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, ud_obj);
    if (ud && ud->id) {
        g_source_remove(ud->id);
        ud->id = 0;
    }
    mrb_hash_delete_key(mrb, fh, fd_obj);
}

/* ---- GTK timers --------------------------------------------------------- */

struct mrb_ascaridol_timer_ud {
    mrb_state* mrb;
    mrb_value  blk;
    guint      id       = 0;
    guint      interval = 0;

    ~mrb_ascaridol_timer_ud() {
        if (id) { g_source_remove(id); id = 0; }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_timer_ud, Ascaridol_Timer);

static gboolean
on_timer_fire(gpointer user_data)
{
    auto* ud = static_cast<mrb_ascaridol_timer_ud*>(user_data);
    mrb_state* mrb = ud->mrb;

    /* Pin the user block across the yield. timer_map keyed by ud->id is
     * the persistent GC root for the owning Ruby Timer; we keep it across
     * the yield, then move/remove on stop or different-interval rearm. */
    mrb_value old_key = mrb_convert_number(mrb, ud->id);
    mrb_value blk     = ud->blk;
    mrb_gc_register(mrb, blk);

    mrb_int ai = mrb_gc_arena_save(mrb);

    mrb_value ret = mrb_yield_argv(mrb, blk, 0, nullptr);

    guint next_ms = 0;
    bool  rearm   = false;
    if (mrb_true_p(ret)) {
        next_ms = ud->interval; rearm = true;
    } else if (!mrb_nil_p(ret) && !mrb_false_p(ret)) {
        uint32_t n_ms;
        mrb_chrono_convert(mrb, ret, MRB_CHRONO_OUT_UINT32,
                           MRB_CHRONO_DUR_MILLISECONDS,
                           MRB_CHRONO_CEIL, &n_ms, sizeof n_ms);
        { next_ms = (guint)n_ms; rearm = true; }
    }

    if (rearm && next_ms == ud->interval) {
        mrb_gc_arena_restore(mrb, ai);
        mrb_gc_unregister(mrb, blk);
        return G_SOURCE_CONTINUE;
    }

    mrb_value timer = mrb_hash_fetch(mrb, timer_map(mrb), old_key, mrb_undef_value());
    mrb_hash_delete_key(mrb, timer_map(mrb), old_key);
    ud->id = 0;
    mrb_gc_arena_restore(mrb, ai);
    if (rearm) {
        ud->interval = next_ms;
        ud->id = g_timeout_add(next_ms, on_timer_fire, ud);
        if (!mrb_undef_p(timer)) {
            mrb_hash_set(mrb, timer_map(mrb),
                mrb_convert_number(mrb, ud->id), timer);
        }
    }
    mrb_gc_unregister(mrb, blk);
    return G_SOURCE_REMOVE;
}

static mrb_value
ascaridol_timer_ud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value blk; mrb_value secs;
    mrb_get_args(mrb, "oo", &secs, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_ascaridol_timer_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    ud->mrb = mrb; ud->blk = blk;
    uint32_t ms;
    mrb_chrono_convert(mrb, secs, MRB_CHRONO_OUT_UINT32,
                       MRB_CHRONO_DUR_MILLISECONDS,
                       MRB_CHRONO_CEIL, &ms, sizeof ms);
    ud->interval = (guint)ms;
    ud->id = g_timeout_add((guint)ms, on_timer_fire, ud);
    mrb_hash_set(mrb, timer_map(mrb), mrb_convert_number(mrb, ud->id), self);
    return self;
}

static mrb_value
mrb_ascaridol_timer_rearm(mrb_state* mrb, mrb_value self)
{
    mrb_value secs; mrb_get_args(mrb, "o", &secs);
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    if (ud->id) {
        mrb_hash_delete_key(mrb, timer_map(mrb), mrb_convert_number(mrb, ud->id));
        g_source_remove(ud->id);
        ud->id = 0;
    }
    uint32_t ms;
    mrb_chrono_convert(mrb, secs, MRB_CHRONO_OUT_UINT32,
                       MRB_CHRONO_DUR_MILLISECONDS,
                       MRB_CHRONO_CEIL, &ms, sizeof ms);
    {
        ud->interval = (guint)ms;
        ud->id = g_timeout_add((guint)ms, on_timer_fire, ud);
        mrb_hash_set(mrb, timer_map(mrb), mrb_convert_number(mrb, ud->id), self);
    }
    return self;
}

static mrb_value
mrb_ascaridol_timer_cancel(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    if (ud->id) {
        mrb_hash_delete_key(mrb, timer_map(mrb), mrb_convert_number(mrb, ud->id));
        g_source_remove(ud->id);
        ud->id = 0;
    }
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_timer_active_p(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    return mrb_bool_value(ud->id != 0);
}
#endif /* WEBVIEW_GTK */

#ifdef WEBVIEW_COCOA
#include <CoreFoundation/CoreFoundation.h>

struct mrb_ascaridol_fd_ud {
    mrb_state* mrb;
    mrb_value fd;
    mrb_value blk;
    CFFileDescriptorRef cf_fd = nullptr;
    CFRunLoopSourceRef  src = nullptr;
    CFOptionFlags       rearm_mask = 0;

    ~mrb_ascaridol_fd_ud() {
        if (src) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
            CFRelease(src);
            src = nullptr;
        }
        if (cf_fd) {
            CFFileDescriptorInvalidate(cf_fd);
            CFRelease(cf_fd);
            cf_fd = nullptr;
        }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_fd_ud, Ascaridol_FDUD);

static void
on_cf_fd_ready(CFFileDescriptorRef cf_fd, CFOptionFlags types, void* info)
{
    auto* ud = static_cast<mrb_ascaridol_fd_ud*>(info);
    mrb_state* mrb = ud->mrb;
    mrb_int ai = mrb_gc_arena_save(mrb);

    unsigned int conds = 0;
    if (types & kCFFileDescriptorReadCallBack)  conds |= ASCARIDOL_FD_READ;
    if (types & kCFFileDescriptorWriteCallBack) conds |= ASCARIDOL_FD_WRITE;

    const mrb_value argv[] = { ud->fd, mrb_symbol_value(ascaridol_cond_to_sym(conds)) };
    mrb_bool cont = mrb_test(mrb_yield_argv(mrb, ud->blk, 2, argv));

    if (cont) {
        CFFileDescriptorEnableCallBacks(cf_fd, ud->rearm_mask);
    }
    else {
        mrb_hash_delete_key(mrb, fds_hash(mrb), ud->fd);
    }
    mrb_gc_arena_restore(mrb, ai);
}

static mrb_value
ascaridol_fdud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value fd, blk;
    mrb_get_args(mrb, "oo", &fd, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(fd), fd);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_ascaridol_fd_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    ud->mrb = mrb;
    ud->fd = fd;
    ud->blk = blk;
    return self;
}

mrb_value
ascaridol_add_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj, mrb_value blk, unsigned int mask)
{
    int fd = (int)mrb_integer(mrb_type_convert(mrb, fd_obj,
        MRB_TT_INTEGER, MRB_SYM(fileno)));

    struct RClass* asc = mrb_module_get_id(mrb, MRB_SYM(Ascaridol));
    struct RClass* watcher_cls = mrb_class_get_under_id(mrb, asc, MRB_SYM(Watcher));

    mrb_value argv[] = { fd_obj, blk };
    mrb_value ud_obj = mrb_obj_new(mrb, watcher_cls, 2, argv);
    mrb_hash_set(mrb, fds_hash(mrb), fd_obj, ud_obj);

    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, ud_obj);

    CFOptionFlags cb = 0;
    if (mask & ASCARIDOL_FD_READ)  cb |= kCFFileDescriptorReadCallBack;
    if (mask & ASCARIDOL_FD_WRITE) cb |= kCFFileDescriptorWriteCallBack;
    ud->rearm_mask = cb;

    CFFileDescriptorContext ctx = { 0, ud, nullptr, nullptr, nullptr };
    ud->cf_fd = CFFileDescriptorCreate(kCFAllocatorDefault, fd,
        /*closeOnInvalidate=*/false,
        on_cf_fd_ready, &ctx);
    if (!ud->cf_fd) {
        mrb_hash_delete_key(mrb, fds_hash(mrb), fd_obj);
        mrb_raise(mrb, E_RUNTIME_ERROR, "CFFileDescriptorCreate failed");
    }
    CFFileDescriptorEnableCallBacks(ud->cf_fd, cb);

    ud->src = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, ud->cf_fd, 0);
    if (!ud->src) {
        mrb_hash_delete_key(mrb, fds_hash(mrb), fd_obj);
        mrb_raise(mrb, E_RUNTIME_ERROR, "CFFileDescriptorCreateRunLoopSource failed");
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), ud->src, kCFRunLoopCommonModes);
    return ud_obj;
}

static mrb_value
mrb_ascaridol_watcher_update(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    if (!ud->cf_fd) mrb_raise(mrb, E_IO_ERROR, "watcher already removed");

    mrb_value readiness;
    mrb_get_args(mrb, "o", &readiness);
    unsigned int mask = ascaridol_parse_readiness(mrb, readiness);

    CFOptionFlags cb = 0;
    if (mask & ASCARIDOL_FD_READ)  cb |= kCFFileDescriptorReadCallBack;
    if (mask & ASCARIDOL_FD_WRITE) cb |= kCFFileDescriptorWriteCallBack;

    CFFileDescriptorDisableCallBacks(ud->cf_fd, ud->rearm_mask);
    ud->rearm_mask = cb;
    CFFileDescriptorEnableCallBacks(ud->cf_fd, cb);
    return self;
}

static mrb_value
mrb_ascaridol_watcher_remove(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    if (!ud->cf_fd) mrb_raise(mrb, E_IO_ERROR, "watcher already removed");
    if (ud->src) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), ud->src, kCFRunLoopCommonModes);
        CFRelease(ud->src);
        ud->src = nullptr;
    }
    CFFileDescriptorInvalidate(ud->cf_fd);
    CFRelease(ud->cf_fd);
    ud->cf_fd = nullptr;
    mrb_hash_delete_key(mrb, fds_hash(mrb), ud->fd);
    return mrb_nil_value();
}

void
ascaridol_remove_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj)
{
    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (!mrb_undef_p(ud_obj)) {
        mrb_hash_delete_key(mrb, fh, fd_obj);  /* dtor handles teardown */
    }
}

/* ---- Cocoa timers ------------------------------------------------------- */

struct mrb_ascaridol_timer_ud {
    mrb_state*        mrb;
    mrb_value         blk;
    CFRunLoopTimerRef timer    = nullptr;
    CFTimeInterval    interval = 0.0;

    ~mrb_ascaridol_timer_ud() {
        if (timer) {
            CFRunLoopRemoveTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
            CFRunLoopTimerInvalidate(timer); CFRelease(timer); timer = nullptr;
        }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_timer_ud, Ascaridol_Timer);

static void ascaridol_arm_cf_timer(mrb_ascaridol_timer_ud*, double, mrb_value);

static void
on_cf_timer_fire(CFRunLoopTimerRef /*ref*/, void* info)
{
    auto* ud = static_cast<mrb_ascaridol_timer_ud*>(info);
    mrb_state* mrb = ud->mrb;

    /* Pin the owning Ruby Timer across the yield. timer_map keyed by the
     * current CFRunLoopTimerRef is its persistent GC root; we save it
     * out, then remove the entry under the soon-invalidated key. If the
     * user rearms, ascaridol_arm_cf_timer re-adds it under the new ref. */
    mrb_value key   = mrb_convert_number(mrb, (uintptr_t)ud->timer);
    mrb_value timer = mrb_hash_fetch(mrb, timer_map(mrb), key, mrb_undef_value());
    if (!mrb_undef_p(timer)) mrb_gc_register(mrb, timer);
    mrb_hash_delete_key(mrb, timer_map(mrb), key);

    mrb_value blk = ud->blk;
    mrb_gc_register(mrb, blk);

    mrb_int ai = mrb_gc_arena_save(mrb);

    CFRunLoopRemoveTimer(CFRunLoopGetMain(), ud->timer, kCFRunLoopCommonModes);
    CFRunLoopTimerInvalidate(ud->timer); CFRelease(ud->timer); ud->timer = nullptr;

    mrb_value ret = mrb_yield_argv(mrb, blk, 0, nullptr);

    double next_ms = -1.0;
    if (mrb_true_p(ret)) {
        next_ms = ud->interval * 1000.0;
    } else if (!mrb_nil_p(ret) && !mrb_false_p(ret)) {
        double n;
        mrb_chrono_convert(mrb, ret, MRB_CHRONO_OUT_DOUBLE,
                           MRB_CHRONO_DUR_MILLISECONDS,
                           MRB_CHRONO_TRUNC, &n, sizeof n);
        if (n >= 0.0) next_ms = n;
    }
    if (next_ms >= 0.0 && !mrb_undef_p(timer)) {
        ascaridol_arm_cf_timer(ud, next_ms, timer);
    }

    mrb_gc_arena_restore(mrb, ai);
    mrb_gc_unregister(mrb, blk);
    if (!mrb_undef_p(timer)) mrb_gc_unregister(mrb, timer);
}

static void
ascaridol_arm_cf_timer(mrb_ascaridol_timer_ud* ud, double ms, mrb_value self)
{
    CFAbsoluteTime fire = CFAbsoluteTimeGetCurrent() + ms / 1000.0;
    CFRunLoopTimerContext ctx = { 0, ud, nullptr, nullptr, nullptr };
    ud->timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        fire, 0, 0, 0, on_cf_timer_fire, &ctx);
    ud->interval = ms / 1000.0;
    mrb_hash_set(ud->mrb, timer_map(ud->mrb),
        mrb_convert_number(ud->mrb, (uintptr_t)ud->timer), self);
    CFRunLoopAddTimer(CFRunLoopGetMain(), ud->timer, kCFRunLoopCommonModes);
}

static mrb_value
ascaridol_timer_ud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value blk; mrb_value secs;
    mrb_get_args(mrb, "oo", &secs, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_ascaridol_timer_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    ud->mrb = mrb; ud->blk = blk;
    double ms_d;
    mrb_chrono_convert(mrb, secs, MRB_CHRONO_OUT_DOUBLE,
                       MRB_CHRONO_DUR_MILLISECONDS,
                       MRB_CHRONO_TRUNC, &ms_d, sizeof ms_d);
    if (ms_d < 0.0) mrb_raise(mrb, E_ARGUMENT_ERROR, "timer interval must be >= 0");
    ascaridol_arm_cf_timer(ud, ms_d, self);
    return self;
}

static mrb_value
mrb_ascaridol_timer_rearm(mrb_state* mrb, mrb_value self)
{
    mrb_value secs; mrb_get_args(mrb, "o", &secs);
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    if (ud->timer) {
        mrb_hash_delete_key(mrb, timer_map(mrb), mrb_convert_number(mrb, (uintptr_t)ud->timer));
        CFRunLoopRemoveTimer(CFRunLoopGetMain(), ud->timer, kCFRunLoopCommonModes);
        CFRunLoopTimerInvalidate(ud->timer); CFRelease(ud->timer); ud->timer = nullptr;
    }
    double ms_d;
    mrb_chrono_convert(mrb, secs, MRB_CHRONO_OUT_DOUBLE,
                       MRB_CHRONO_DUR_MILLISECONDS,
                       MRB_CHRONO_TRUNC, &ms_d, sizeof ms_d);
    if (ms_d >= 0.0) ascaridol_arm_cf_timer(ud, ms_d, self);
    return self;
}

static mrb_value
mrb_ascaridol_timer_cancel(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    if (ud->timer) {
        mrb_hash_delete_key(mrb, timer_map(mrb), mrb_convert_number(mrb, (uintptr_t)ud->timer));
        CFRunLoopRemoveTimer(CFRunLoopGetMain(), ud->timer, kCFRunLoopCommonModes);
        CFRunLoopTimerInvalidate(ud->timer); CFRelease(ud->timer); ud->timer = nullptr;
    }
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_timer_active_p(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    return mrb_bool_value(ud->timer != nullptr);
}
#endif /* WEBVIEW_COCOA */

#ifdef WEBVIEW_EDGE

#define MRB_ASCARIDOL_WM_FD (WM_APP + 1)
static const wchar_t MRB_ASCARIDOL_WND_CLASS[] = L"ascaridol_fd_dispatcher";

struct mrb_ascaridol_fd_ud {
    mrb_state* mrb;
    mrb_value  fd;
    mrb_value  blk;
    int        sock = -1;
    HWND       hwnd = nullptr;

    ~mrb_ascaridol_fd_ud() {
        if (sock != -1 && hwnd && IsWindow(hwnd)) {
            WSAAsyncSelect((SOCKET)sock, hwnd, 0, 0);
        }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_fd_ud, Ascaridol_FDUD);

struct mrb_ascaridol_wnd_ctx {
    mrb_state* mrb = nullptr;
    HWND       hwnd = nullptr;

    ~mrb_ascaridol_wnd_ctx() {
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_wnd_ctx, Ascaridol_WndCtx);

static mrb_value
sockmap_hash(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value h = mrb_iv_get(mrb, asc, MRB_SYM(_native_event_sockmap));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, asc, MRB_SYM(_native_event_sockmap), h);
    }
    return h;
}


static UINT_PTR g_next_timer_id = 1;
static void ascaridol_on_win32_timer(mrb_state*, UINT_PTR);

static LRESULT CALLBACK
ascaridol_fd_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER) {
        auto* ctx2 = reinterpret_cast<mrb_ascaridol_wnd_ctx*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (ctx2 && ctx2->mrb) ascaridol_on_win32_timer(ctx2->mrb, (UINT_PTR)wParam);
        return 0;
    }
    if (msg != MRB_ASCARIDOL_WM_FD) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* ctx = reinterpret_cast<mrb_ascaridol_wnd_ctx*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ctx) return 0;
    mrb_state* mrb = ctx->mrb;

    int sock = (int)wParam;
    int err = WSAGETSELECTERROR(lParam);
    int evt = WSAGETSELECTEVENT(lParam);

    mrb_int ai = mrb_gc_arena_save(mrb);

    mrb_value smap = sockmap_hash(mrb);
    mrb_value sk_v = mrb_int_value(mrb, sock);
    mrb_value fd_obj = mrb_hash_fetch(mrb, smap, sk_v, mrb_undef_value());
    if (mrb_undef_p(fd_obj)) { mrb_gc_arena_restore(mrb, ai); return 0; }

    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (mrb_undef_p(ud_obj)) { mrb_gc_arena_restore(mrb, ai); return 0; }

    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, ud_obj);
    if (!ud) { mrb_gc_arena_restore(mrb, ai); return 0; }

    unsigned int conds = 0;
    if (evt & (FD_READ | FD_OOB | FD_ACCEPT | FD_CLOSE)) conds |= ASCARIDOL_FD_READ;
    if (evt & (FD_WRITE | FD_CONNECT))                   conds |= ASCARIDOL_FD_WRITE;
    if (err != 0)                                        conds |= ASCARIDOL_FD_ERROR;
    const mrb_value argv[] = { ud->fd, mrb_symbol_value(ascaridol_cond_to_sym(conds)) };
    mrb_bool cont = mrb_test(mrb_yield_argv(mrb, ud->blk, 2, argv));

    if (!cont || (evt & FD_CLOSE) || err != 0) {
        if (ud->sock != -1) {
            WSAAsyncSelect((SOCKET)ud->sock, hwnd, 0, 0);
            mrb_hash_delete_key(mrb, smap, sk_v);
            ud->sock = -1;
        }
        mrb_hash_delete_key(mrb, fh, fd_obj);
    }
    mrb_gc_arena_restore(mrb, ai);
    return 0;
}

static mrb_value
ascaridol_wnd_ctx_init(mrb_state* mrb, mrb_value self)
{
    mrb_cpp_new<mrb_ascaridol_wnd_ctx>(mrb, self);
    auto* ctx = mrb_cpp_get<mrb_ascaridol_wnd_ctx>(mrb, self);
    ctx->mrb = mrb;
    return self;
}

static HWND
get_or_create_ascaridol_wnd(mrb_state* mrb)
{
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value ctx_v = mrb_iv_get(mrb, asc, MRB_SYM(_native_event_wnd_ctx));
    mrb_ascaridol_wnd_ctx* ctx = nullptr;
    if (mrb_data_p(ctx_v)) {
        ctx = mrb_cpp_get<mrb_ascaridol_wnd_ctx>(mrb, ctx_v);
        if (ctx && ctx->hwnd) return ctx->hwnd;
    }

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    if (!GetClassInfoExW(hinst, MRB_ASCARIDOL_WND_CLASS, &wc)) {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ascaridol_fd_wnd_proc;
        wc.hInstance = hinst;
        wc.lpszClassName = MRB_ASCARIDOL_WND_CLASS;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            mrb_raisef(mrb, E_RUNTIME_ERROR,
                "RegisterClassEx failed: %d", (mrb_int)GetLastError());
        }
    }

    if (!ctx) {
        struct RClass* asc_cls = mrb_module_get_id(mrb, MRB_SYM(Ascaridol));
        struct RClass* ctx_cls = mrb_class_get_under_id(mrb, asc_cls, MRB_SYM(_WndCtx));
        ctx_v = mrb_obj_new(mrb, ctx_cls, 0, nullptr);
        mrb_iv_set(mrb, asc, MRB_SYM(_native_event_wnd_ctx), ctx_v);
        ctx = mrb_cpp_get<mrb_ascaridol_wnd_ctx>(mrb, ctx_v);
    }

    HWND hwnd = CreateWindowExW(0, MRB_ASCARIDOL_WND_CLASS, L"",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, nullptr);
    if (!hwnd) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "CreateWindowEx (HWND_MESSAGE) failed: %d",
            (mrb_int)GetLastError());
    }
    ctx->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    return hwnd;
}

static mrb_value
ascaridol_fdud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value fd, blk;
    mrb_get_args(mrb, "oo", &fd, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(fd), fd);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_ascaridol_fd_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    ud->mrb = mrb;
    ud->fd = fd;
    ud->blk = blk;
    return self;
}

mrb_value
ascaridol_add_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj, mrb_value blk, unsigned int mask)
{
    int fd = (int)mrb_integer(mrb_type_convert(mrb, fd_obj,
        MRB_TT_INTEGER, MRB_SYM(fileno)));

    int sotype = 0;
    int solen = sizeof(sotype);
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE,
        reinterpret_cast<char*>(&sotype), &solen) == SOCKET_ERROR) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "Ascaridol.add_native_event on Windows requires a winsock SOCKET "
            "(WSAGetLastError=%d)", (mrb_int)WSAGetLastError());
    }

    HWND hwnd = get_or_create_ascaridol_wnd(mrb);

    struct RClass* asc = mrb_module_get_id(mrb, MRB_SYM(Ascaridol));
    struct RClass* watcher_cls = mrb_class_get_under_id(mrb, asc, MRB_SYM(Watcher));
    mrb_value argv[] = { fd_obj, blk };
    mrb_value ud_obj = mrb_obj_new(mrb, watcher_cls, 2, argv);
    mrb_hash_set(mrb, fds_hash(mrb), fd_obj, ud_obj);

    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, ud_obj);
    ud->sock = fd;
    ud->hwnd = hwnd;

    mrb_hash_set(mrb, sockmap_hash(mrb), mrb_int_value(mrb, fd), fd_obj);

    long lEvent = 0;
    if (mask & ASCARIDOL_FD_READ)  lEvent |= FD_READ | FD_OOB | FD_ACCEPT;
    if (mask & ASCARIDOL_FD_WRITE) lEvent |= FD_WRITE | FD_CONNECT;
    lEvent |= FD_CLOSE;

    if (WSAAsyncSelect((SOCKET)fd, hwnd, MRB_ASCARIDOL_WM_FD, lEvent) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        mrb_hash_delete_key(mrb, sockmap_hash(mrb), mrb_int_value(mrb, fd));
        mrb_hash_delete_key(mrb, fds_hash(mrb), fd_obj);
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "WSAAsyncSelect failed: %d", (mrb_int)err);
    }
    return ud_obj;
}

static mrb_value
mrb_ascaridol_watcher_update(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    if (ud->sock == -1) mrb_raise(mrb, E_IO_ERROR, "watcher already removed");

    mrb_value readiness;
    mrb_get_args(mrb, "o", &readiness);
    unsigned int mask = ascaridol_parse_readiness(mrb, readiness);

    long lEvent = 0;
    if (mask & ASCARIDOL_FD_READ)  lEvent |= FD_READ | FD_OOB | FD_ACCEPT;
    if (mask & ASCARIDOL_FD_WRITE) lEvent |= FD_WRITE | FD_CONNECT;
    lEvent |= FD_CLOSE;

    if (WSAAsyncSelect((SOCKET)ud->sock, ud->hwnd, MRB_ASCARIDOL_WM_FD, lEvent) == SOCKET_ERROR) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "WSAAsyncSelect failed: %d", (mrb_int)WSAGetLastError());
    }
    return self;
}

static mrb_value
mrb_ascaridol_watcher_remove(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, self);
    if (ud->sock == -1) mrb_raise(mrb, E_IO_ERROR, "watcher already removed");
    WSAAsyncSelect((SOCKET)ud->sock, ud->hwnd, 0, 0);
    mrb_hash_delete_key(mrb, sockmap_hash(mrb), mrb_int_value(mrb, ud->sock));
    mrb_hash_delete_key(mrb, fds_hash(mrb), ud->fd);
    ud->sock = -1;
    return mrb_nil_value();
}

void
ascaridol_remove_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj)
{
    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (mrb_undef_p(ud_obj)) return;

    auto* ud = mrb_cpp_get<mrb_ascaridol_fd_ud>(mrb, ud_obj);
    if (ud && ud->sock != -1 && ud->hwnd) {
        WSAAsyncSelect((SOCKET)ud->sock, ud->hwnd, 0, 0);
        mrb_hash_delete_key(mrb, sockmap_hash(mrb), mrb_int_value(mrb, ud->sock));
        ud->sock = -1;
    }
    mrb_hash_delete_key(mrb, fh, fd_obj);
}

/* ---- Win32 timers ------------------------------------------------------- */

struct mrb_ascaridol_timer_ud {
    mrb_state* mrb;
    mrb_value  blk;
    HWND       hwnd     = nullptr;
    UINT_PTR   timer_id = 0;
    UINT       interval = 0;

    ~mrb_ascaridol_timer_ud() {
        if (timer_id && hwnd) { KillTimer(hwnd, timer_id); timer_id = 0; }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_ascaridol_timer_ud, Ascaridol_Timer);

static void
ascaridol_on_win32_timer(mrb_state* mrb, UINT_PTR timer_id)
{
    mrb_value key    = mrb_convert_number(mrb, timer_id);
    mrb_value tm_obj = mrb_hash_fetch(mrb, timer_map(mrb), key, mrb_undef_value());
    if (mrb_undef_p(tm_obj)) return;

    /* Pin tm_obj across the yield. timer_map was the only persistent GC
     * root for the Ascaridol::Timer Ruby object and we're about to remove
     * the entry. Without this pin, the user's block can trigger a GC that
     * collects tm_obj, runs ~mrb_ascaridol_timer_ud(), and leaves `ud`
     * dangling for the rest of this function. */
    mrb_gc_register(mrb, tm_obj);

    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, tm_obj);

    KillTimer(ud->hwnd, ud->timer_id);
    ud->timer_id = 0;
    mrb_hash_delete_key(mrb, timer_map(mrb), key);

    mrb_int ai = mrb_gc_arena_save(mrb);
    mrb_value ret = mrb_yield_argv(mrb, ud->blk, 0, nullptr);

    UINT next_ms = 0; bool rearm = false;
    if (mrb_true_p(ret)) {
        next_ms = ud->interval; rearm = true;
    } else if (!mrb_nil_p(ret) && !mrb_false_p(ret)) {
        uint32_t n_ms;
        mrb_chrono_convert(mrb, ret, MRB_CHRONO_OUT_UINT32,
                           MRB_CHRONO_DUR_MILLISECONDS,
                           MRB_CHRONO_CEIL, &n_ms, sizeof n_ms);
        { next_ms = (UINT)n_ms; rearm = true; }
    }

    if (rearm) {
        ud->interval = next_ms; ud->timer_id = g_next_timer_id++;
        mrb_hash_set(mrb, timer_map(mrb),
            mrb_convert_number(mrb, ud->timer_id), tm_obj);
        SetTimer(ud->hwnd, ud->timer_id, next_ms, nullptr);
    }
    mrb_gc_arena_restore(mrb, ai);
    mrb_gc_unregister(mrb, tm_obj);
}

static mrb_value
ascaridol_timer_ud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value blk; mrb_value secs;
    mrb_get_args(mrb, "oo", &secs, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_ascaridol_timer_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    ud->mrb = mrb; ud->blk = blk;
    ud->hwnd = get_or_create_ascaridol_wnd(mrb);
    uint32_t ms;
    mrb_chrono_convert(mrb, secs, MRB_CHRONO_OUT_UINT32,
                       MRB_CHRONO_DUR_MILLISECONDS,
                       MRB_CHRONO_CEIL, &ms, sizeof ms);
    ud->interval = (UINT)ms; ud->timer_id = g_next_timer_id++;
    mrb_hash_set(mrb, timer_map(mrb),
        mrb_convert_number(mrb, ud->timer_id), self);
    SetTimer(ud->hwnd, ud->timer_id, (UINT)ms, nullptr);
    return self;
}

static mrb_value
mrb_ascaridol_timer_rearm(mrb_state* mrb, mrb_value self)
{
    mrb_value secs; mrb_get_args(mrb, "o", &secs);
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    if (ud->timer_id) {
        KillTimer(ud->hwnd, ud->timer_id);
        mrb_hash_delete_key(mrb, timer_map(mrb),
            mrb_convert_number(mrb, ud->timer_id));
        ud->timer_id = 0;
    }
    uint32_t ms;
    mrb_chrono_convert(mrb, secs, MRB_CHRONO_OUT_UINT32,
                       MRB_CHRONO_DUR_MILLISECONDS,
                       MRB_CHRONO_CEIL, &ms, sizeof ms);
    {
        ud->interval = (UINT)ms; ud->timer_id = g_next_timer_id++;
        mrb_hash_set(mrb, timer_map(mrb),
            mrb_convert_number(mrb, ud->timer_id), self);
        SetTimer(ud->hwnd, ud->timer_id, (UINT)ms, nullptr);
    }
    return self;
}

static mrb_value
mrb_ascaridol_timer_cancel(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    if (ud->timer_id) {
        KillTimer(ud->hwnd, ud->timer_id);
        mrb_hash_delete_key(mrb, timer_map(mrb),
            mrb_convert_number(mrb, ud->timer_id));
        ud->timer_id = 0;
    }
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_timer_active_p(mrb_state* mrb, mrb_value self)
{
    auto* ud = mrb_cpp_get<mrb_ascaridol_timer_ud>(mrb, self);
    return mrb_bool_value(ud->timer_id != 0);
}
#endif /* WEBVIEW_EDGE */


/* ========================================================================= */
/* Ascaridol.add_timer                                                       */
/* ========================================================================= */

static mrb_value
mrb_ascaridol_add_timer(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.add_timer");
    mrb_value secs;
    mrb_value blk = mrb_undef_value();
    mrb_get_args(mrb, "o&!", &secs, &blk);
    struct RClass* asc       = mrb_module_get_id(mrb, MRB_SYM(Ascaridol));
    struct RClass* timer_cls = mrb_class_get_under_id(mrb, asc, MRB_SYM(Timer));
    mrb_value argv[] = { secs, blk };
    return mrb_obj_new(mrb, timer_cls, 2, argv);
}

/* ASCARIDOL_TAURI_MENU_V1 */
/* ASCARIDOL_MENU_COMPILE_FIXUP_V1 */
/* Forward declaration — defined in the lifecycle section further down. */
static webview::webview* ascaridol_require_running_local(mrb_state* mrb);

/* ========================================================================= */
/* Ruby-driven native menu                                                   */
/*                                                                           */
/* Ascaridol.menu = [[group_label, [item, ...]], ...] builds a real native  */
/* menu bar for the current backend. Items reference Ruby bindings by sym;  */
/* native activation invokes the proc directly on main — no JS hop.         */
/*                                                                           */
/*   Item shapes:                                                            */
/*     [label, :bind_sym]                  basic                            */
/*     [label, :bind_sym, "CmdOrCtrl+S"]   with accelerator                 */
/*     [:separator]                        separator line                   */
/*                                                                           */
/* Accelerator "CmdOrCtrl" translates to Cmd on macOS, Ctrl elsewhere.      */
/* ========================================================================= */

/* ASCARIDOL_GTK_ACCEL_SYNTAX_V1 */
static std::string
ascaridol_translate_accel(const char* s)
{
    if (!s || !*s) return {};

#if defined(WEBVIEW_GTK)
    /* GTK wants "<Control><Shift>s" form for gtk_accelerator_parse and for
     * the GMenuItem accel attribute display. Split on '+', wrap modifiers
     * in <>, lowercase the key. */
    std::vector<std::string> parts;
    {
        std::string buf;
        auto flush = [&]() {
            while (!buf.empty() && std::isspace((unsigned char)buf.front())) buf.erase(0, 1);
            while (!buf.empty() && std::isspace((unsigned char)buf.back()))  buf.pop_back();
            if (!buf.empty()) parts.push_back(buf);
            buf.clear();
        };
        for (const char* p = s; *p; p++) { if (*p == '+') flush(); else buf += *p; }
        flush();
    }
    if (parts.empty()) return {};

    std::string out;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        std::string upper = parts[i];
        for (char& c : upper) c = (char)std::toupper((unsigned char)c);
        if (upper == "CMDORCTRL" || upper == "CTRL" || upper == "CONTROL" ||
            upper == "CMD" || upper == "COMMAND") {
            out += "<Control>";
        } else if (upper == "SHIFT") {
            out += "<Shift>";
        } else if (upper == "ALT" || upper == "OPT" || upper == "OPTION") {
            out += "<Alt>";
        } else if (upper == "SUPER" || upper == "META") {
            out += "<Super>";
        } else {
            out += "<" + parts[i] + ">";
        }
    }
    std::string key = parts.back();
    /* Single-char keys are lowercased; named keys (F1, Return, plus, minus...)
     * are accepted by gtk_accelerator_parse in any case but lowercase is
     * the canonical form. */
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    out += key;
    return out;
#else
    /* Win32 and Cocoa: keep the "Ctrl+Q" / "Cmd+Q" shape that their own
     * parsers (parse_win32_accel / parse_cocoa_accel) understand. Just
     * swap "CmdOrCtrl" for the platform's primary modifier. */
    std::string out(s);
#  if defined(WEBVIEW_COCOA)
    const char* repl = "Cmd";
#  else
    const char* repl = "Ctrl";
#  endif
    size_t pos = 0;
    while ((pos = out.find("CmdOrCtrl", pos)) != std::string::npos) {
        out.replace(pos, 9, repl);
        pos += strlen(repl);
    }
    return out;
#endif
}

struct ascaridol_menu_trigger_ctx { mrb_value proc; };

static mrb_value
ascaridol_menu_trigger_body(mrb_state* mrb, void* p)
{
    auto* ctx = static_cast<ascaridol_menu_trigger_ctx*>(p);
    return mrb_yield_argv(mrb, ctx->proc, 0, nullptr);
}

/* Look up the proc bound under bind_sym in Ascaridol.@bindings and call it.
 * Errors are printed and swallowed — menu activation is a UI event, not a
 * Ruby caller. Main thread only. */
static void
ascaridol_trigger_menu_action(mrb_sym bind_sym)
{
    mrb_state* mrb = g_main_mrb.load(std::memory_order_acquire);
    if (!mrb) return;

    mrb_int ai = mrb_gc_arena_save(mrb);
    mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_value bh = mrb_iv_get(mrb, asc, MRB_SYM(bindings));
    if (mrb_hash_p(bh)) {
        mrb_value proc = mrb_hash_fetch(mrb, bh,
            mrb_symbol_value(bind_sym), mrb_undef_value());
        if (mrb_proc_p(proc)) {
            ascaridol_menu_trigger_ctx ctx{ proc };
            mrb_bool err = FALSE;
            mrb_protect_error(mrb, ascaridol_menu_trigger_body, &ctx, &err);
            if (err && mrb->exc) {
                mrb_print_error(mrb);
                mrb->exc = nullptr;
            }
        }
    }
    mrb_gc_arena_restore(mrb, ai);
}

struct AscaridolMenuItemSpec {
    bool is_separator = false;
    std::string label;
    mrb_sym bind_sym = 0;
    std::string accel;
};

static bool
ascaridol_parse_item(mrb_state* mrb, mrb_value item, AscaridolMenuItemSpec& out)
{
    if (!mrb_array_p(item)) return false;
    mrb_int len = RARRAY_LEN(item);
    if (len < 1) return false;

    mrb_value first = mrb_ary_ref(mrb, item, 0);
    if (mrb_symbol_p(first) && mrb_symbol(first) == MRB_SYM(separator)) {
        out.is_separator = true;
        return true;
    }
    if (len < 2) return false;
    if (!mrb_string_p(first)) return false;
    mrb_value sym_v = mrb_ary_ref(mrb, item, 1);
    if (!mrb_symbol_p(sym_v)) return false;

    out.label = std::string(RSTRING_PTR(first), RSTRING_LEN(first));
    out.bind_sym = mrb_symbol(sym_v);

    if (len > 2) {
        mrb_value accel_v = mrb_ary_ref(mrb, item, 2);
        if (mrb_string_p(accel_v)) {
            std::string raw(RSTRING_PTR(accel_v), RSTRING_LEN(accel_v));
            out.accel = ascaridol_translate_accel(raw.c_str());
        }
    }
    return true;
}

/* ---- Win32 -------------------------------------------------------------- */

#if defined(WEBVIEW_EDGE)

#include <WebView2.h>

struct AscaridolWin32Accel {
    UINT vkey  = 0;
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
};

struct AscaridolWin32MenuItem {
    UINT id;
    mrb_sym bind_sym;
    AscaridolWin32Accel accel;
};

static std::vector<AscaridolWin32MenuItem> g_win32_menu_items;

static AscaridolWin32Accel
parse_win32_accel(const char* s)
{
    AscaridolWin32Accel a;
    std::string buf;
    auto flush = [&]() {
        while (!buf.empty() && std::isspace((unsigned char)buf.front())) buf.erase(0, 1);
        while (!buf.empty() && std::isspace((unsigned char)buf.back()))  buf.pop_back();
        if (buf.empty()) return;
        std::string upper = buf;
        for (char& c : upper) c = (char)std::toupper((unsigned char)c);
        if      (upper == "CTRL" || upper == "CONTROL" || upper == "CMD" || upper == "COMMAND") a.ctrl = true;
        else if (upper == "SHIFT") a.shift = true;
        else if (upper == "ALT" || upper == "OPT" || upper == "OPTION" || upper == "MENU") a.alt = true;
        else if (upper == "F1")  a.vkey = VK_F1;
        else if (upper == "F2")  a.vkey = VK_F2;
        else if (upper == "F3")  a.vkey = VK_F3;
        else if (upper == "F4")  a.vkey = VK_F4;
        else if (upper == "F5")  a.vkey = VK_F5;
        else if (upper == "F6")  a.vkey = VK_F6;
        else if (upper == "F7")  a.vkey = VK_F7;
        else if (upper == "F8")  a.vkey = VK_F8;
        else if (upper == "F9")  a.vkey = VK_F9;
        else if (upper == "F10") a.vkey = VK_F10;
        else if (upper == "F11") a.vkey = VK_F11;
        else if (upper == "F12") a.vkey = VK_F12;
        else if (upper == "ENTER" || upper == "RETURN")    a.vkey = VK_RETURN;
        else if (upper == "ESC"   || upper == "ESCAPE")    a.vkey = VK_ESCAPE;
        else if (upper == "TAB")                           a.vkey = VK_TAB;
        else if (upper == "SPACE")                         a.vkey = VK_SPACE;
        else if (upper == "DEL"   || upper == "DELETE")    a.vkey = VK_DELETE;
        else if (upper == "BACK"  || upper == "BACKSPACE") a.vkey = VK_BACK;
        else if (upper == "LEFT")   a.vkey = VK_LEFT;
        else if (upper == "RIGHT")  a.vkey = VK_RIGHT;
        else if (upper == "UP")     a.vkey = VK_UP;
        else if (upper == "DOWN")   a.vkey = VK_DOWN;
        else if (upper == "HOME")   a.vkey = VK_HOME;
        else if (upper == "END")    a.vkey = VK_END;
        else if (upper == "PLUS")   a.vkey = VK_OEM_PLUS;
        else if (upper == "MINUS")  a.vkey = VK_OEM_MINUS;
        else if (buf.size() == 1) {
            SHORT scan = VkKeyScanA(buf[0]);
            if (scan != -1) a.vkey = (UINT)(scan & 0xFF);
        }
        buf.clear();
    };
    for (; *s; ++s) { if (*s == '+') flush(); else buf += *s; }
    flush();
    return a;
}

class AscaridolKeyHandler : public ICoreWebView2AcceleratorKeyPressedEventHandler {
    LONG m_refs = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown ||
            riid == __uuidof(ICoreWebView2AcceleratorKeyPressedEventHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2Controller* /*sender*/,
        ICoreWebView2AcceleratorKeyPressedEventArgs* args) override
    {
        COREWEBVIEW2_KEY_EVENT_KIND kind;
        if (FAILED(args->get_KeyEventKind(&kind))) return S_OK;
        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
            kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;

        UINT vkey;
        if (FAILED(args->get_VirtualKey(&vkey))) return S_OK;

        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;

        for (auto& it : g_win32_menu_items) {
            if (it.bind_sym == 0)             continue;
            if (it.accel.vkey == 0)           continue;
            if (it.accel.vkey  != vkey)       continue;
            if (it.accel.ctrl  != ctrl)       continue;
            if (it.accel.shift != shift)      continue;
            if (it.accel.alt   != alt)        continue;
            args->put_Handled(TRUE);
            ascaridol_trigger_menu_action(it.bind_sym);
            return S_OK;
        }
        return S_OK;
    }
};

static bool g_win32_menu_subclass_installed = false;
static bool g_win32_accel_handler_installed = false;
static const UINT MRB_ASCARIDOL_MENU_ID_BASE = 0xA000;

static LRESULT CALLBACK
ascaridol_menu_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR)
{
    if (msg == WM_COMMAND && HIWORD(wParam) == 0) {
        UINT id = LOWORD(wParam);
        for (auto& it : g_win32_menu_items) {
            if (it.id == id && it.bind_sym != 0) {
                ascaridol_trigger_menu_action(it.bind_sym);
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void
ascaridol_install_menu_native(webview::webview* wv, mrb_state* mrb, mrb_value spec)
{
    auto window_result = wv->window();
    if (!window_result.ok()) return;
    HWND hwnd = static_cast<HWND>(window_result.value());
    if (!hwnd || !IsWindow(hwnd)) return;

    g_win32_menu_items.clear();
    HMENU bar = CreateMenu();
    if (!bar) return;

    for (mrb_int i = 0; i < RARRAY_LEN(spec); i++) {
        mrb_value group = mrb_ary_ref(mrb, spec, i);
        if (!mrb_array_p(group) || RARRAY_LEN(group) < 2) continue;
        mrb_value glabel_v = mrb_ary_ref(mrb, group, 0);
        if (!mrb_string_p(glabel_v)) continue;
        const char* g_lbl = RSTRING_CSTR(mrb, glabel_v);
        mrb_value items   = mrb_ary_ref(mrb, group, 1);
        if (!mrb_array_p(items)) continue;

        HMENU sub = CreatePopupMenu();
        for (mrb_int j = 0; j < RARRAY_LEN(items); j++) {
            AscaridolMenuItemSpec it;
            if (!ascaridol_parse_item(mrb, mrb_ary_ref(mrb, items, j), it)) continue;

            if (it.is_separator) {
                AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                continue;
            }

            UINT id = MRB_ASCARIDOL_MENU_ID_BASE + (UINT)g_win32_menu_items.size();
            AscaridolWin32MenuItem entry;
            entry.id = id;
            entry.bind_sym = it.bind_sym;
            if (!it.accel.empty()) entry.accel = parse_win32_accel(it.accel.c_str());
            g_win32_menu_items.push_back(std::move(entry));

            std::string display = it.label;
            if (!it.accel.empty()) { display += "\t"; display += it.accel; }
            AppendMenuA(sub, MF_STRING, id, display.c_str());
        }
        AppendMenuA(bar, MF_POPUP, (UINT_PTR)sub, g_lbl);
    }

    HMENU old = GetMenu(hwnd);
    if (!SetMenu(hwnd, bar)) { DestroyMenu(bar); return; }
    if (old) DestroyMenu(old);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    if (!g_win32_menu_subclass_installed) {
        SetWindowSubclass(hwnd, ascaridol_menu_subclass_proc, 1, 0);
        g_win32_menu_subclass_installed = true;
    }

    if (!g_win32_accel_handler_installed) {
        auto bc_result = wv->browser_controller();
        if (bc_result.ok() && bc_result.value()) {
            auto* controller = static_cast<ICoreWebView2Controller*>(bc_result.value());
            EventRegistrationToken token;
            auto* h = new AscaridolKeyHandler();
            if (SUCCEEDED(controller->add_AcceleratorKeyPressed(h, &token))) {
                g_win32_accel_handler_installed = true;
            }
            h->Release();
        }
    }
}

static void
attach_menu_and_icon(webview::webview* wv)
{
    auto window_result = wv->window();
    if (!window_result.ok()) return;
    HWND hwnd = static_cast<HWND>(window_result.value());
    if (!hwnd || !IsWindow(hwnd)) return;

    HINSTANCE hinst = GetModuleHandleA(nullptr);
    HICON small_icon = (HICON)LoadImageA(hinst, MAKEINTRESOURCEA(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    HICON big_icon = (HICON)LoadImageA(hinst, MAKEINTRESOURCEA(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (small_icon) SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    if (big_icon)   SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big_icon);
}

/* ---- Cocoa -------------------------------------------------------------- */

#elif defined(WEBVIEW_COCOA)

#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <cctype>

namespace {
inline id  oc_cls (const char* n)                   { return (id)objc_getClass(n); }
inline id  oc_call(id o, const char* s)             { return ((id(*)(id,SEL))objc_msgSend)(o, sel_registerName(s)); }
inline id  oc_call(id o, const char* s, id a)       { return ((id(*)(id,SEL,id))objc_msgSend)(o, sel_registerName(s), a); }
inline void oc_void(id o, const char* s, id a)      { ((void(*)(id,SEL,id))objc_msgSend)(o, sel_registerName(s), a); }
inline void oc_void(id o, const char* s, unsigned long a) { ((void(*)(id,SEL,unsigned long))objc_msgSend)(o, sel_registerName(s), a); }
inline id  ns_str (const char* u)                   { return ((id(*)(id,SEL,const char*))objc_msgSend)(oc_cls("NSString"), sel_registerName("stringWithUTF8String:"), u ? u : ""); }
inline id  ns_num_ll(long long v)                   { return ((id(*)(id,SEL,long long))objc_msgSend)(oc_cls("NSNumber"), sel_registerName("numberWithLongLong:"), v); }
inline long long ns_num_get(id n)                   { return ((long long(*)(id,SEL))objc_msgSend)(n, sel_registerName("longLongValue")); }
}

static const unsigned long ASCARIDOL_MOD_SHIFT   = 1UL << 17;
static const unsigned long ASCARIDOL_MOD_CONTROL = 1UL << 18;
static const unsigned long ASCARIDOL_MOD_OPTION  = 1UL << 19;
static const unsigned long ASCARIDOL_MOD_COMMAND = 1UL << 20;

static void
parse_cocoa_accel(const char* s, std::string& key_eq, unsigned long& mods)
{
    key_eq.clear();
    mods = 0;
    std::string buf;
    auto flush = [&]() {
        while (!buf.empty() && std::isspace((unsigned char)buf.front())) buf.erase(0,1);
        while (!buf.empty() && std::isspace((unsigned char)buf.back()))  buf.pop_back();
        if (buf.empty()) return;
        if      (buf == "Cmd" || buf == "Command")                 mods |= ASCARIDOL_MOD_COMMAND;
        else if (buf == "Ctrl"|| buf == "Control")                 mods |= ASCARIDOL_MOD_CONTROL;
        else if (buf == "Opt" || buf == "Option" || buf == "Alt")  mods |= ASCARIDOL_MOD_OPTION;
        else if (buf == "Shift")                                   mods |= ASCARIDOL_MOD_SHIFT;
        else {
            key_eq.clear();
            for (char c : buf) key_eq += (char)std::tolower((unsigned char)c);
        }
        buf.clear();
    };
    for (; *s; ++s) { if (*s == '+') flush(); else buf += *s; }
    flush();
}

static id g_ascaridol_menu_target = nullptr;

static void
ascaridol_menu_trigger_imp(id /*self*/, SEL /*_cmd*/, id sender)
{
    id repobj = oc_call(sender, "representedObject");
    if (!repobj) return;
    mrb_sym bind_sym = (mrb_sym)ns_num_get(repobj);
    if (bind_sym == 0) return;
    ascaridol_trigger_menu_action(bind_sym);
}

static void
ensure_ascaridol_menu_target()
{
    if (g_ascaridol_menu_target) return;
    Class c = objc_allocateClassPair(objc_getClass("NSObject"), "AscaridolMenuTarget", 0);
    class_addMethod(c, sel_registerName("ascaridolTrigger:"),
                    (IMP)ascaridol_menu_trigger_imp, "v@:@");
    objc_registerClassPair(c);
    g_ascaridol_menu_target = oc_call((id)c, "new");
}

static void
ascaridol_install_menu_native(webview::webview* /*wv*/, mrb_state* mrb, mrb_value spec)
{
    ensure_ascaridol_menu_target();

    id menubar = oc_call(oc_call(oc_cls("NSMenu"), "alloc"), "initWithTitle:", ns_str(""));

    /* App-menu placeholder at index 0 — Cocoa hides the first user submenu otherwise. */
    id app_item = oc_call(oc_call(oc_cls("NSMenuItem"), "alloc"), "init");
    id app_sub  = oc_call(oc_call(oc_cls("NSMenu"), "alloc"), "initWithTitle:", ns_str(""));
    oc_void(app_item, "setSubmenu:", app_sub);
    oc_void(menubar, "addItem:", app_item);

    for (mrb_int i = 0; i < RARRAY_LEN(spec); i++) {
        mrb_value group = mrb_ary_ref(mrb, spec, i);
        if (!mrb_array_p(group) || RARRAY_LEN(group) < 2) continue;
        mrb_value glabel_v = mrb_ary_ref(mrb, group, 0);
        if (!mrb_string_p(glabel_v)) continue;
        const char* g_lbl = RSTRING_CSTR(mrb, glabel_v);
        mrb_value items   = mrb_ary_ref(mrb, group, 1);
        if (!mrb_array_p(items)) continue;

        id sub = oc_call(oc_call(oc_cls("NSMenu"), "alloc"), "initWithTitle:", ns_str(g_lbl));
        id grp = oc_call(oc_call(oc_cls("NSMenuItem"), "alloc"), "init");
        oc_void(grp, "setTitle:", ns_str(g_lbl));
        oc_void(grp, "setSubmenu:", sub);
        oc_void(menubar, "addItem:", grp);

        for (mrb_int j = 0; j < RARRAY_LEN(items); j++) {
            AscaridolMenuItemSpec it;
            if (!ascaridol_parse_item(mrb, mrb_ary_ref(mrb, items, j), it)) continue;

            if (it.is_separator) {
                id sep = ((id(*)(id,SEL))objc_msgSend)(
                    oc_cls("NSMenuItem"), sel_registerName("separatorItem"));
                oc_void(sub, "addItem:", sep);
                continue;
            }

            std::string key_eq;
            unsigned long mods = 0;
            if (!it.accel.empty()) parse_cocoa_accel(it.accel.c_str(), key_eq, mods);

            id mi = ((id(*)(id,SEL,id,SEL,id))objc_msgSend)(
                oc_call(oc_cls("NSMenuItem"), "alloc"),
                sel_registerName("initWithTitle:action:keyEquivalent:"),
                ns_str(it.label.c_str()),
                sel_registerName("ascaridolTrigger:"),
                ns_str(key_eq.c_str()));
            oc_void(mi, "setKeyEquivalentModifierMask:", mods);
            oc_void(mi, "setTarget:", g_ascaridol_menu_target);
            oc_void(mi, "setRepresentedObject:", ns_num_ll((long long)it.bind_sym));
            oc_void(sub, "addItem:", mi);
        }
    }

    id nsapp = oc_call(oc_cls("NSApplication"), "sharedApplication");
    oc_void(nsapp, "setMainMenu:", menubar);
}

static void attach_menu_and_icon(webview::webview*) {}

/* ---- GTK (3 and 4) ------------------------------------------------------ */

#elif defined(WEBVIEW_GTK)

#include <gtk/gtk.h>

static GtkWindow* g_menu_window         = nullptr;
static GtkWidget* g_menu_webview_widget = nullptr;
static GtkWidget* g_menu_box            = nullptr;
static GtkWidget* g_menu_bar_widget     = nullptr;

#if GTK_MAJOR_VERSION >= 4

static GtkEventController* g_menu_shortcut_ctrl = nullptr;

static void
ascaridol_action_trigger(GSimpleAction*, GVariant* param, gpointer)
{
    if (!param) return;
    if (!g_variant_is_of_type(param, G_VARIANT_TYPE_INT64)) return;
    mrb_sym bind_sym = (mrb_sym)g_variant_get_int64(param);
    if (bind_sym == 0) return;
    ascaridol_trigger_menu_action(bind_sym);
}

static void
ascaridol_menu_first_time_setup(webview::webview* wv)
{
    if (g_menu_box) return;
    g_menu_window         = GTK_WINDOW(wv->window().value());
    g_menu_webview_widget = GTK_WIDGET(wv->widget().value());

    GSimpleActionGroup* grp = g_simple_action_group_new();
    GSimpleAction* trigger = g_simple_action_new("trigger", G_VARIANT_TYPE_INT64);
    g_signal_connect(trigger, "activate", G_CALLBACK(ascaridol_action_trigger), nullptr);
    g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(trigger));
    g_object_unref(trigger);
    gtk_widget_insert_action_group(GTK_WIDGET(g_menu_window), "ascaridol", G_ACTION_GROUP(grp));
    g_object_unref(grp);

    g_menu_shortcut_ctrl = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(
        GTK_SHORTCUT_CONTROLLER(g_menu_shortcut_ctrl), GTK_SHORTCUT_SCOPE_GLOBAL);
    gtk_widget_add_controller(GTK_WIDGET(g_menu_window), g_menu_shortcut_ctrl);

    g_object_ref(g_menu_webview_widget);
    gtk_window_set_child(g_menu_window, nullptr);
    g_menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(g_menu_webview_widget, TRUE);
    gtk_widget_set_hexpand(g_menu_webview_widget, TRUE);
    gtk_box_append(GTK_BOX(g_menu_box), g_menu_webview_widget);
    g_object_unref(g_menu_webview_widget);
    gtk_window_set_child(g_menu_window, g_menu_box);
}

static void
ascaridol_clear_shortcuts()
{
    if (!g_menu_shortcut_ctrl) return;
    GListModel* m = G_LIST_MODEL(g_menu_shortcut_ctrl);
    for (guint n = g_list_model_get_n_items(m); n > 0; n--) {
        GtkShortcut* sc = GTK_SHORTCUT(g_list_model_get_item(m, n - 1));
        gtk_shortcut_controller_remove_shortcut(
            GTK_SHORTCUT_CONTROLLER(g_menu_shortcut_ctrl), sc);
        g_object_unref(sc);
    }
}

static void
ascaridol_install_menu_native(webview::webview* wv, mrb_state* mrb, mrb_value spec)
{
    ascaridol_menu_first_time_setup(wv);
    ascaridol_clear_shortcuts();

    GMenu* bar = g_menu_new();
    for (mrb_int i = 0; i < RARRAY_LEN(spec); i++) {
        mrb_value group = mrb_ary_ref(mrb, spec, i);
        if (!mrb_array_p(group) || RARRAY_LEN(group) < 2) continue;
        mrb_value glabel_v = mrb_ary_ref(mrb, group, 0);
        if (!mrb_string_p(glabel_v)) continue;
        const char* g_lbl = RSTRING_CSTR(mrb, glabel_v);
        mrb_value items   = mrb_ary_ref(mrb, group, 1);
        if (!mrb_array_p(items)) continue;

        GMenu* sub = g_menu_new();
        GMenu* current = sub;
        bool current_owned_by_sub = true;

        for (mrb_int j = 0; j < RARRAY_LEN(items); j++) {
            AscaridolMenuItemSpec it;
            if (!ascaridol_parse_item(mrb, mrb_ary_ref(mrb, items, j), it)) continue;

            if (it.is_separator) {
                GMenu* next = g_menu_new();
                g_menu_append_section(sub, nullptr, G_MENU_MODEL(next));
                if (!current_owned_by_sub) g_object_unref(current);
                current = next;
                current_owned_by_sub = false;
                continue;
            }

            GMenuItem* mi = g_menu_item_new(it.label.c_str(), nullptr);
            g_menu_item_set_action_and_target_value(mi, "ascaridol.trigger",
                g_variant_new_int64((gint64)it.bind_sym));

            if (!it.accel.empty()) {
                /* ASCARIDOL_GTK4_ACCEL_DISPLAY_V1 */
                guint key = 0;
                GdkModifierType mods = (GdkModifierType)0;
                gtk_accelerator_parse(it.accel.c_str(), &key, &mods);
                if (key) {
                    /* Canonicalize for the menu attribute — GtkPopoverMenuBar
                     * renders the label from this and wants "<Control>q"
                     * form, not "Ctrl+Q". */
                    gchar* gtk_name = gtk_accelerator_name(key, mods);
                    g_menu_item_set_attribute_value(mi, "accel",
                        g_variant_new_string(gtk_name));
                    g_free(gtk_name);

                    GtkShortcut* sc = gtk_shortcut_new(
                        gtk_keyval_trigger_new(key, mods),
                        gtk_named_action_new("ascaridol.trigger"));
                    gtk_shortcut_set_arguments(sc,
                        g_variant_new_int64((gint64)it.bind_sym));
                    gtk_shortcut_controller_add_shortcut(
                        GTK_SHORTCUT_CONTROLLER(g_menu_shortcut_ctrl), sc);
                    /* ASCARIDOL_GTK4_NUMPAD_V1 */
                    guint kp_equiv = 0;
                    if      (key == GDK_KEY_plus  || key == GDK_KEY_equal) kp_equiv = GDK_KEY_KP_Add;
                    else if (key == GDK_KEY_minus)                         kp_equiv = GDK_KEY_KP_Subtract;
                    else if (key == GDK_KEY_asterisk)                      kp_equiv = GDK_KEY_KP_Multiply;
                    else if (key == GDK_KEY_slash)                         kp_equiv = GDK_KEY_KP_Divide;
                    if (kp_equiv) {
                        GtkShortcut* kp_sc = gtk_shortcut_new(
                            gtk_keyval_trigger_new(kp_equiv, mods),
                            gtk_named_action_new("ascaridol.trigger"));
                        gtk_shortcut_set_arguments(kp_sc,
                            g_variant_new_int64((gint64)it.bind_sym));
                        gtk_shortcut_controller_add_shortcut(
                            GTK_SHORTCUT_CONTROLLER(g_menu_shortcut_ctrl), kp_sc);
                    }
                }
            }

            g_menu_append_item(current, mi);
            g_object_unref(mi);
        }
        if (!current_owned_by_sub) g_object_unref(current);

        g_menu_append_submenu(bar, g_lbl, G_MENU_MODEL(sub));
        g_object_unref(sub);
    }

    GtkWidget* new_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    g_object_unref(bar);
    if (g_menu_bar_widget) {
        gtk_box_remove(GTK_BOX(g_menu_box), g_menu_bar_widget);
    }
    gtk_box_insert_child_after(GTK_BOX(g_menu_box), new_bar, nullptr);
    g_menu_bar_widget = new_bar;
}

#else /* GTK 3 */

static GtkAccelGroup* g_menu_accel_group = nullptr;

static void
ascaridol_on_item_activate(GtkMenuItem* item, gpointer)
{
    mrb_sym bind_sym = (mrb_sym)(uintptr_t)g_object_get_data(G_OBJECT(item),
        "ascaridol-bind-sym");
    if (bind_sym == 0) return;
    ascaridol_trigger_menu_action(bind_sym);
}

static void
ascaridol_menu_first_time_setup(webview::webview* wv)
{
    if (g_menu_box) return;
    g_menu_window         = GTK_WINDOW(wv->window().value());
    g_menu_webview_widget = GTK_WIDGET(wv->widget().value());

    g_menu_accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(g_menu_window, g_menu_accel_group);

    g_object_ref(g_menu_webview_widget);
    GtkWidget* current = gtk_bin_get_child(GTK_BIN(g_menu_window));
    if (current) gtk_container_remove(GTK_CONTAINER(g_menu_window), current);
    g_menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(g_menu_box), g_menu_webview_widget, TRUE, TRUE, 0);
    g_object_unref(g_menu_webview_widget);
    gtk_container_add(GTK_CONTAINER(g_menu_window), g_menu_box);
    gtk_widget_show_all(g_menu_box);
}

static void
ascaridol_install_menu_native(webview::webview* wv, mrb_state* mrb, mrb_value spec)
{
    ascaridol_menu_first_time_setup(wv);

    GtkWidget* bar = gtk_menu_bar_new();
    for (mrb_int i = 0; i < RARRAY_LEN(spec); i++) {
        mrb_value group = mrb_ary_ref(mrb, spec, i);
        if (!mrb_array_p(group) || RARRAY_LEN(group) < 2) continue;
        mrb_value glabel_v = mrb_ary_ref(mrb, group, 0);
        if (!mrb_string_p(glabel_v)) continue;
        const char* g_lbl = RSTRING_CSTR(mrb, glabel_v);
        mrb_value items   = mrb_ary_ref(mrb, group, 1);
        if (!mrb_array_p(items)) continue;

        GtkWidget* group_item = gtk_menu_item_new_with_label(g_lbl);
        GtkWidget* sub = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(group_item), sub);

        for (mrb_int j = 0; j < RARRAY_LEN(items); j++) {
            AscaridolMenuItemSpec it;
            if (!ascaridol_parse_item(mrb, mrb_ary_ref(mrb, items, j), it)) continue;

            if (it.is_separator) {
                GtkWidget* sep = gtk_separator_menu_item_new();
                gtk_menu_shell_append(GTK_MENU_SHELL(sub), sep);
                continue;
            }

            GtkWidget* mi = gtk_menu_item_new_with_label(it.label.c_str());
            g_object_set_data(G_OBJECT(mi), "ascaridol-bind-sym",
                (gpointer)(uintptr_t)it.bind_sym);
            g_signal_connect(mi, "activate",
                G_CALLBACK(ascaridol_on_item_activate), nullptr);

            if (!it.accel.empty()) {
                /* ASCARIDOL_GTK3_NUMPAD_V1 */
                guint key = 0;
                GdkModifierType mods = (GdkModifierType)0;
                gtk_accelerator_parse(it.accel.c_str(), &key, &mods);
                if (key) {
                    gtk_widget_add_accelerator(mi, "activate",
                        g_menu_accel_group, key, mods, GTK_ACCEL_VISIBLE);
                    /* Also bind numpad equivalents. */
                    guint kp_equiv = 0;
                    if      (key == GDK_KEY_plus  || key == GDK_KEY_equal) kp_equiv = GDK_KEY_KP_Add;
                    else if (key == GDK_KEY_minus)                         kp_equiv = GDK_KEY_KP_Subtract;
                    else if (key == GDK_KEY_asterisk)                      kp_equiv = GDK_KEY_KP_Multiply;
                    else if (key == GDK_KEY_slash)                         kp_equiv = GDK_KEY_KP_Divide;
                    if (kp_equiv) {
                        gtk_widget_add_accelerator(mi, "activate",
                            g_menu_accel_group, kp_equiv, mods,
                            (GtkAccelFlags)0);
                    }
                }
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), group_item);
    }

    if (g_menu_bar_widget) {
        gtk_container_remove(GTK_CONTAINER(g_menu_box), g_menu_bar_widget);
    }
    gtk_box_pack_start(GTK_BOX(g_menu_box), bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(g_menu_box), bar, 0);
    gtk_widget_show_all(bar);
    g_menu_bar_widget = bar;
}

#endif /* GTK_MAJOR_VERSION */

static void attach_menu_and_icon(webview::webview*) {}

#else
static void attach_menu_and_icon(webview::webview*) {}
static void ascaridol_install_menu_native(webview::webview*, mrb_state*,
    mrb_value) {}
#endif

/* ---- Ascaridol.menu= — Ruby-facing setter ------------------------------- */

static mrb_value
mrb_ascaridol_menu_set(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.menu=");
    mrb_value spec;
    mrb_get_args(mrb, "A", &spec);

    webview::webview* wv = ascaridol_require_running_local(mrb);
    ascaridol_install_menu_native(wv, mrb, spec);
    return spec;
}

/* ========================================================================= */
/* Ascaridol.run, Ascaridol.ready, Ascaridol.enable_html_menu                */
/* ========================================================================= */

static webview::webview*
ascaridol_require_running_local(mrb_state* mrb)
{
    webview::webview* wv = g_wv.load(std::memory_order_acquire);
    if (!wv) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "Ascaridol is not running");
    }
    return wv;
}

static webview_hint_t
hint_from_kw(mrb_state* mrb, mrb_value v)
{
    if (mrb_nil_p(v))    return WEBVIEW_HINT_NONE;
    if (!mrb_symbol_p(v)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "size hint must be a Symbol");
    }
    mrb_sym s = mrb_symbol(v);
    if (s == MRB_SYM(none))  return WEBVIEW_HINT_NONE;
    if (s == MRB_SYM(min))   return WEBVIEW_HINT_MIN;
    if (s == MRB_SYM(max))   return WEBVIEW_HINT_MAX;
    if (s == MRB_SYM(fixed)) return WEBVIEW_HINT_FIXED;
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown size hint: %v", v);
    return WEBVIEW_HINT_NONE;
}

#define ASCARIDOL_PAGE_READY_BINDING "__ascaridol_page_ready_internal__"

static mrb_value
mrb_ascaridol_ready(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.ready");
    mrb_value blk = mrb_undef_value();
    mrb_get_args(mrb, "&", &blk);
    if (mrb_undef_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }
    if (!mrb_proc_p(blk)) {
        mrb_raise(mrb, E_TYPE_ERROR, "not a block");
    }
    if (g_ready_hook_set) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "Ascaridol.ready hook already set");
    }
    mrb_gc_register(mrb, blk);
    g_ready_hook = blk;
    g_ready_hook_set = true;
    return mrb_nil_value();
}

static void
ascaridol_reset_for_run(mrb_state* mrb)
{
    mrb_value asc_mod = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
    mrb_iv_set(mrb, asc_mod, MRB_SYM(bindings),        mrb_hash_new(mrb));
    mrb_iv_set(mrb, asc_mod, MRB_SYM(async_bindings),  mrb_hash_new(mrb));
    mrb_iv_set(mrb, asc_mod, MRB_SYM(fds_procs),       mrb_hash_new(mrb));
    mrb_iv_set(mrb, asc_mod, MRB_SYM(_timer_map),      mrb_hash_new(mrb));
    g_ready_fired = false;

    /* Menu state — webview destruction invalidated the widgets; reset
     * pointers so the next run starts fresh. */
#if defined(WEBVIEW_GTK)
    g_menu_window         = nullptr;
    g_menu_webview_widget = nullptr;
    g_menu_box            = nullptr;
    g_menu_bar_widget     = nullptr;
#  if GTK_MAJOR_VERSION >= 4
    g_menu_shortcut_ctrl  = nullptr;
#  else
    g_menu_accel_group    = nullptr;
#  endif
#elif defined(WEBVIEW_EDGE)
    g_win32_menu_items.clear();
    g_win32_menu_subclass_installed = false;
    g_win32_accel_handler_installed = false;
#endif
}

static mrb_value
mrb_ascaridol_run(mrb_state* mrb, mrb_value self)
{
    if (g_wv.load(std::memory_order_acquire) != nullptr) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "Ascaridol is already running");
    }
    if (!ascaridol_in_main_state(mrb)) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
            "Ascaridol.run must be called from the main thread mruby vm");
    }

    mrb_value kw_title = mrb_undef_value();
    mrb_value kw_size = mrb_undef_value();
    mrb_value kw_html = mrb_undef_value();
    mrb_value kw_url = mrb_undef_value();
    mrb_value kw_init = mrb_undef_value();

    const mrb_sym kw_names[] = {
        MRB_SYM(title), MRB_SYM(size),
        MRB_SYM(html),  MRB_SYM(url),  MRB_SYM(init)
    };
    mrb_value kw_values[5];
    mrb_kwargs kwargs = {
        5, 0, kw_names, kw_values, nullptr
    };

    mrb_value blk = mrb_undef_value();
    mrb_get_args(mrb, ":&", &kwargs, &blk);
    if (mrb_undef_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }
    if (!mrb_proc_p(blk)) {
        mrb_raise(mrb, E_TYPE_ERROR, "not a block");
    }

    if (!mrb_undef_p(kw_values[0])) kw_title = kw_values[0];
    if (!mrb_undef_p(kw_values[1])) kw_size = kw_values[1];
    if (!mrb_undef_p(kw_values[2])) kw_html = kw_values[2];
    if (!mrb_undef_p(kw_values[3])) kw_url = kw_values[3];
    if (!mrb_undef_p(kw_values[4])) kw_init = kw_values[4];

    if (!mrb_undef_p(kw_html) && !mrb_undef_p(kw_url)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR,
            "Ascaridol.run accepts either html: or url:, not both");
    }

    struct RClass* owner_cls = mrb_class_get_under_id(mrb,
        mrb_module_get_id(mrb, MRB_SYM(Ascaridol)), MRB_SYM(_WVOwner));
    mrb_value owner_val = mrb_obj_new(mrb, owner_cls, 0, nullptr);
    auto* owner = mrb_cpp_new<AscaridolWVOwner>(mrb, owner_val);
    mrb_gc_register(mrb, owner_val);

    try {
#ifdef MRB_DEBUG
        owner->wv = new webview::webview(true, nullptr);
#else
        owner->wv = new webview::webview(false, nullptr);
#endif
    }
    catch (const webview::exception& e) {
        mrb_gc_unregister(mrb, owner_val);
        mrb_cpp_delete(mrb, owner);
        ascaridol_check(mrb, e.error().code(), e.error().message());
        return mrb_nil_value();
    }
    webview::webview* wv = owner->wv;
    g_wv.store(wv, std::memory_order_release);

    ascaridol_reset_for_run(mrb);

    if (mrb_string_p(kw_title)) {
        ascaridol_check_result(mrb, wv->set_title(to_std_string(kw_title)));
    }
    if (mrb_array_p(kw_size)) {
        mrb_int n = RARRAY_LEN(kw_size);
        if (n < 2 || n > 3) {
            mrb_raise(mrb, E_ARGUMENT_ERROR,
                "size: kwarg must be [w, h] or [w, h, hint]");
        }
        mrb_int w = mrb_integer(mrb_to_int(mrb, mrb_ary_ref(mrb, kw_size, 0)));
        mrb_int h = mrb_integer(mrb_to_int(mrb, mrb_ary_ref(mrb, kw_size, 1)));
        webview_hint_t hint = (n == 3)
            ? hint_from_kw(mrb, mrb_ary_ref(mrb, kw_size, 2))
            : WEBVIEW_HINT_NONE;
        ascaridol_check_result(mrb, wv->set_size((int)w, (int)h, hint));
    }
    if (mrb_string_p(kw_init)) {
        ascaridol_check_result(mrb, wv->init(to_std_string(kw_init)));
    }
    if (mrb_string_p(kw_html)) {
        ascaridol_check_result(mrb, wv->set_html(to_std_string(kw_html)));
    }
    else if (mrb_string_p(kw_url)) {
        ascaridol_check_result(mrb, wv->navigate(to_std_string(kw_url)));
    }

    /* Win32-only: set the window icon. (Menus are opt-in via
     * Ascaridol.enable_html_menu.) */
    attach_menu_and_icon(wv);

    /* Install the DOMContentLoaded listener and bind _ascaridol_ready
     * BEFORE yielding the setup block so that html= / url= called
     * inside the block see them on the first navigation. */
    ascaridol_check_result(mrb, wv->init(
        "(function(){"
        "  document.addEventListener('DOMContentLoaded',function(){"
        "    if(typeof window._ascaridol_ready==='function'){"
        "      window._ascaridol_ready().catch(function(){});"
        "    }"
        "  });"
        "})();"
    ));

    if (g_ready_hook_set) {
        mrb_value name  = mrb_symbol_value(MRB_SYM(_ascaridol_ready));
        mrb_value asc = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Ascaridol)));
        mrb_funcall_with_block(mrb, asc, MRB_SYM(bind), 1, &name, g_ready_hook);
    }

    mrb_value ascaridol_module = mrb_obj_value(mrb_class_ptr(self));
    struct ctx_2 { mrb_value blk; mrb_value arg; };
    ctx_2 c2{ blk, ascaridol_module };

    mrb_bool err = FALSE;
    mrb_value exc = mrb_protect_error(mrb,
        [](mrb_state* m, void* p) -> mrb_value {
            ctx_2* c2 = static_cast<ctx_2*>(p);
            return mrb_yield_argv(m, c2->blk, 1, &c2->arg);
        },
        &c2, &err);

    if (err) {
        mrb_gc_unregister(mrb, owner_val);
        mrb_cpp_delete(mrb, owner);
        mrb_data_init(owner_val, nullptr, nullptr);
        if (mrb_obj_is_kind_of(mrb, exc, mrb->eException_class)) {
            mrb_exc_raise(mrb, exc);
        }
        else {
            mrb_raise(mrb, E_RUNTIME_ERROR,
                "Ascaridol.run setup block raised a non-exception value");
        }
        return mrb_nil_value();
    }

    using run_result_t = decltype(wv->run());
    run_result_t run_result{};

    struct ctx { webview::webview* wv; run_result_t* out; };
    ctx c{ wv, &run_result };

    mrb_bool run_err = FALSE;
    mrb_value run_exc = mrb_protect_error(mrb,
        [](mrb_state* m, void* p) -> mrb_value {
            ctx* c = static_cast<ctx*>(p);
            *c->out = c->wv->run();
            return mrb_nil_value();
        },
        &c, &run_err);

    mrb_gc_unregister(mrb, owner_val);
    mrb_cpp_delete(mrb, owner);
    mrb_data_init(owner_val, nullptr, nullptr);

    if (run_err) {
        if (mrb_obj_is_kind_of(mrb, run_exc, mrb->eException_class)) {
            mrb_exc_raise(mrb, run_exc);
        }
        else {
            mrb_raise(mrb, E_RUNTIME_ERROR,
                "Ascaridol run loop raised a non-exception value");
        }
        return mrb_nil_value();
    }

    ascaridol_check_result(mrb, run_result);
    return self;
}

/* ========================================================================= */
/* Main-only Ascaridol.* methods                                             */
/* ========================================================================= */

static mrb_value
mrb_ascaridol_bind(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.bind");

    mrb_sym name_sym;
    mrb_value blk = mrb_undef_value();
    mrb_get_args(mrb, "n&", &name_sym, &blk);
    if (mrb_undef_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }
    if (!mrb_proc_p(blk)) {
        mrb_raise(mrb, E_TYPE_ERROR, "not a block");
    }

    mrb_int name_len;
    const char* name_s = mrb_sym_name_len(mrb, name_sym, &name_len);
    std::string name(name_s, static_cast<size_t>(name_len));

    webview::webview* wv = ascaridol_require_running_local(mrb);
    ascaridol_bind_on_main(mrb, wv, name_sym, name, blk);
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_bind_async(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.bind_async");

    mrb_sym name_sym;
    mrb_value blk = mrb_undef_value();
    mrb_get_args(mrb, "n&", &name_sym, &blk);
    if (mrb_undef_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }
    if (!mrb_proc_p(blk)) {
        mrb_raise(mrb, E_TYPE_ERROR, "not a block");
    }

    mrb_int name_len;
    const char* name_s = mrb_sym_name_len(mrb, name_sym, &name_len);
    std::string name(name_s, static_cast<size_t>(name_len));

    webview::webview* wv = ascaridol_require_running_local(mrb);
    ascaridol_bind_async_on_main(mrb, wv, name_sym, name, blk);
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_unbind(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.unbind");

    mrb_sym name_sym;
    mrb_get_args(mrb, "n", &name_sym);

    mrb_int name_len;
    const char* name_s = mrb_sym_name_len(mrb, name_sym, &name_len);
    std::string name(name_s, static_cast<size_t>(name_len));

    webview::webview* wv = ascaridol_require_running_local(mrb);
    ascaridol_unbind_on_main(mrb, wv, name_sym, name);
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_add_native_event(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.add_native_event");

    mrb_value fd_obj;
    mrb_value readiness = mrb_nil_value();
    mrb_value blk = mrb_undef_value();
    mrb_get_args(mrb, "o|o&", &fd_obj, &readiness, &blk);
    if (mrb_undef_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }
    if (!mrb_proc_p(blk)) {
        mrb_raise(mrb, E_TYPE_ERROR, "not a block");
    }
    unsigned int mask = ascaridol_parse_readiness(mrb, readiness);

    webview::webview* wv = ascaridol_require_running_local(mrb);
    return ascaridol_add_native_event_on_main(mrb, wv, fd_obj, blk, mask);
}

static mrb_value
mrb_ascaridol_remove_native_event(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.remove_native_event");

    mrb_value fd_obj;
    mrb_get_args(mrb, "o", &fd_obj);

    webview::webview* wv = ascaridol_require_running_local(mrb);
    ascaridol_remove_native_event_on_main(mrb, wv, fd_obj);
    return mrb_nil_value();
}

static mrb_value
mrb_ascaridol_bindings(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.bindings");
    return ascaridol_bindings_on_main(mrb);
}

static mrb_value
mrb_ascaridol_handle(mrb_state* mrb, mrb_value /*self*/)
{
    ascaridol_require_main_state(mrb, "Ascaridol.handle");
    mrb_value kind = mrb_symbol_value(MRB_SYM(window));
    mrb_get_args(mrb, "|o", &kind);
    webview::webview* wv = ascaridol_require_running_local(mrb);
    return ascaridol_native_handle_on_main(mrb, wv, kind);
}

/* ========================================================================= */
/* Gem extension: register lifecycle + main-only methods.                    */
/* ========================================================================= */

static void
ascaridol_install_runtime(mrb_state* mrb)
{
    struct RClass* asc = mrb_module_get_id(mrb, MRB_SYM(Ascaridol));

    /* Error class hierarchy. */
    struct RClass* err = mrb_define_class_under_id(mrb, asc,
        MRB_SYM(Error),
        E_RUNTIME_ERROR);
    mrb_define_class_under_id(mrb, asc, MRB_SYM(MissingDependencyError), err);
    mrb_define_class_under_id(mrb, asc, MRB_SYM(CanceledError), err);
    mrb_define_class_under_id(mrb, asc, MRB_SYM(InvalidStateError), err);
    mrb_define_class_under_id(mrb, asc, MRB_SYM(InvalidArgumentError), err);
    mrb_define_class_under_id(mrb, asc, MRB_SYM(DuplicateError), err);
    mrb_define_class_under_id(mrb, asc, MRB_SYM(NotFoundError), err);

    /* Ascaridol::Watcher. */
    struct RClass* watcher_cls = mrb_define_class_under_id(mrb, asc,
        MRB_SYM(Watcher),
        mrb->object_class);
    MRB_SET_INSTANCE_TT(watcher_cls, MRB_TT_CDATA);
    mrb_define_method_id(mrb, watcher_cls, MRB_SYM(initialize),
        ascaridol_fdud_init, MRB_ARGS_REQ(2));
    mrb_define_method_id(mrb, watcher_cls, MRB_SYM(io),
        mrb_ascaridol_watcher_io, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, watcher_cls, MRB_SYM(update),
        mrb_ascaridol_watcher_update, MRB_ARGS_REQ(1));
    mrb_define_method_id(mrb, watcher_cls, MRB_SYM(remove),
        mrb_ascaridol_watcher_remove, MRB_ARGS_NONE());

#ifdef WEBVIEW_EDGE
    struct RClass* ctx_cls = mrb_define_class_under_id(mrb, asc,
        MRB_SYM(_WndCtx),
        mrb->object_class);
    MRB_SET_INSTANCE_TT(ctx_cls, MRB_TT_CDATA);
    mrb_define_method_id(mrb, ctx_cls, MRB_SYM(initialize),
        ascaridol_wnd_ctx_init, MRB_ARGS_NONE());
#endif

    {
        struct RClass* owner_cls = mrb_define_class_under_id(mrb, asc,
            MRB_SYM(_WVOwner), mrb->object_class);
        MRB_SET_INSTANCE_TT(owner_cls, MRB_TT_CDATA);
    }

    /* Lifecycle entry points. */
    mrb_define_class_method_id(mrb, asc, MRB_SYM(run),
        mrb_ascaridol_run,
        MRB_ARGS_KEY(5, 0) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, asc, MRB_SYM(ready),
        mrb_ascaridol_ready, MRB_ARGS_BLOCK());

    /* Main-only methods that touch mrb_state-resident state. */
    mrb_define_class_method_id(mrb, asc, MRB_SYM(bind),
        mrb_ascaridol_bind,
        MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, asc, MRB_SYM(bind_async),
        mrb_ascaridol_bind_async,
        MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, asc, MRB_SYM(unbind),
        mrb_ascaridol_unbind, MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, asc, MRB_SYM(add_native_event),
        mrb_ascaridol_add_native_event,
        MRB_ARGS_ARG(1, 1) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, asc, MRB_SYM(poll_add),
        mrb_ascaridol_add_native_event,
        MRB_ARGS_ARG(1, 1) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, asc, MRB_SYM(remove_native_event),
        mrb_ascaridol_remove_native_event,
        MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, asc, MRB_SYM(poll_remove),
        mrb_ascaridol_remove_native_event,
        MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, asc, MRB_SYM(bindings),
        mrb_ascaridol_bindings, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, asc, MRB_SYM(handle),
        mrb_ascaridol_handle, MRB_ARGS_OPT(1));


    /* Ascaridol::Timer */
    {
        struct RClass* timer_cls = mrb_define_class_under_id(mrb, asc,
            MRB_SYM(Timer), mrb->object_class);
        MRB_SET_INSTANCE_TT(timer_cls, MRB_TT_CDATA);
        mrb_define_method_id(mrb, timer_cls, MRB_SYM(initialize),
            ascaridol_timer_ud_init, MRB_ARGS_REQ(2));
        mrb_define_method_id(mrb, timer_cls, MRB_SYM(rearm),
            mrb_ascaridol_timer_rearm, MRB_ARGS_REQ(1));
        mrb_define_method_id(mrb, timer_cls, MRB_SYM(cancel),
            mrb_ascaridol_timer_cancel, MRB_ARGS_NONE());
        mrb_define_method_id(mrb, timer_cls, MRB_SYM(active_q),
            mrb_ascaridol_timer_active_p, MRB_ARGS_NONE());
    }
    mrb_define_class_method_id(mrb, asc, MRB_SYM(add_timer),
        mrb_ascaridol_add_timer, MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());

    /* Ruby-driven native menu. */
    mrb_define_class_method_id(mrb, asc, MRB_SYM_E(menu),
        mrb_ascaridol_menu_set, MRB_ARGS_REQ(1));
}

#ifdef _WIN32
struct WSAGuard {
    bool ok = false;
    int  err = 0;
    WSAGuard() { WSADATA w; err = WSAStartup(MAKEWORD(2, 2), &w); ok = (err == 0); }
    ~WSAGuard() { if (ok) WSACleanup(); }
};
#endif

/* ========================================================================= */
/* main()                                                                    */
/* ========================================================================= */
#include <mruby/debug.h>
static void
trace_fetch(mrb_state* mrb, const struct mrb_irep* irep,
            const mrb_code* pc, mrb_value* /*regs*/)
{
    const char* file = mrb_debug_get_filename(mrb, irep, (uint32_t)(pc - irep->iseq));
    int32_t line     = mrb_debug_get_line(mrb, irep, (uint32_t)(pc - irep->iseq));
    if (file && line >= 0) {
        fprintf(stderr, "[trace] %s:%d\n", file, line);
    }
}

int
main(const int argc, const char* const argv[])
{
#ifdef _WIN32
    WSAGuard wsa;
    if (!wsa.ok) { fprintf(stderr, "WSAStartup failed: %d\n", wsa.err); return 1; }
#endif
    if (argc > 1 && std::string_view(argv[1]) == "--prewarm") {
        try { webview::webview w(false, nullptr); }
        catch (const std::exception& e) {
            fprintf(stderr, "prewarm failed: %s\n", e.what());
            return 1;
        }
        return 0;
    }
    mrb_state* mrb = mrb_open();
    if (!mrb) return 1;
    g_main_mrb.store(mrb, std::memory_order_release);
    mrb->code_fetch_hook = trace_fetch;

    int exit_code = 0;

    {
        mrb_value ARGV = mrb_ary_new_capa(mrb, argc);
        for (int i = 0; i < argc; i++) {
            mrb_value arg = mrb_str_new_static_frozen(mrb, argv[i],
                strlen(argv[i]));
            mrb_ary_push(mrb, ARGV, arg);
        }
        mrb_obj_freeze(mrb, ARGV);
        mrb_define_const_id(mrb, mrb->object_class, MRB_SYM(ARGV), ARGV);
    }

    ascaridol_install_runtime(mrb);
    
    mrb_funcall_argv(mrb, mrb_top_self(mrb), MRB_SYM(main), 0, nullptr);
    if (mrb->exc) {
        mrb_print_error(mrb);
        exit_code = 1;
    }

    g_main_mrb.store(nullptr, std::memory_order_release);
    mrb_close(mrb);
    return exit_code;
}