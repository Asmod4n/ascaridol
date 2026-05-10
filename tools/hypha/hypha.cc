/*
 * tools/hypha/hypha.cpp — Hypha runtime: main(), Hypha.run, Hypha.ready,
 *                         and all the platform-specific work that wraps
 *                         the live webview.
 *
 * Architecture recap:
 *
 *   src/hypha_methods.cpp defines Hypha.title=, .html=, .bind, .dispatch,
 *   .add_native_event, etc. Each method dispatches off-main calls onto
 *   main via webview::dispatch, capturing CBOR-serialized payloads as
 *   plain std::strings into the dispatch lambdas. On main, the dispatched
 *   lambda calls into one of the hypha_*_on_main helpers defined here.
 *
 *   This file:
 *     - Defines the globals (g_main_mrb, g_wv).
 *     - Implements the on_main helpers, which run with full access to
 *       the live webview and main's mrb_state.
 *     - Defines Hypha.run (kwarg-driven setup, yields the Hypha module
 *       to a block, runs ready hook, then webview::run blocks).
 *     - Defines Hypha.ready (one-shot post-setup hook).
 *     - Owns the C main() that opens mrb, exposes ARGV, runs the
 *       embedded user script, cleans up.
 *
 * The live webview is constructed inside Hypha.run on main, stored in
 * g_wv with release semantics so workers can see it. It's destroyed at
 * the end of Hypha.run, with g_wv nulled (release) before the destructor
 * runs.
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
#include <mruby/presym.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <mruby/compile.h>
#include <mruby/irep.h>

#include <mruby/cpp_helpers.hpp>
#include <mruby/fast_json.h>

#include <webview/webview.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>

#include "../../src/webview_internal.h"

extern "C" const uint8_t hypha_main[];  /* embedded user script, mrbc-compiled */

/* ========================================================================= */
/* Lifecycle state.                                                          */
/*                                                                           */
/* g_main_mrb and g_wv are defined in src/hypha_methods.cpp (so they sit in */
/* libmruby.lib and other binaries can link cleanly). We read/write them    */
/* here via the externs in webview_internal.h.                              */
/* ========================================================================= */

/* Hypha.ready holds at most one block. Set during the user's setup
 * (before or inside Hypha.run's block), fired exactly once after the
 * setup block returns and before webview::run starts pumping. */
static mrb_value g_ready_hook;
static bool      g_ready_hook_set = false;

/* Sticky once-per-process flag. Set when Hypha.run begins; never cleared.
 * Protects against the macOS NSApplication-singleton hazard and similar. */
static bool g_hypha_used = false;

/* ========================================================================= */
/* Error class lookup                                                        */
/* ========================================================================= */

MRB_CPP_DEFINE_TYPE(webview::webview, Webview)

static struct RClass*
hypha_error_class(mrb_state* mrb, webview_error_t err)
{
    struct RClass* base = mrb_module_get_id(mrb, MRB_SYM(Hypha));
    mrb_sym name;
    switch (err) {
    case WEBVIEW_ERROR_MISSING_DEPENDENCY: name = MRB_SYM(MissingDependencyError); break;
    case WEBVIEW_ERROR_CANCELED:           name = MRB_SYM(CanceledError);          break;
    case WEBVIEW_ERROR_INVALID_STATE:      name = MRB_SYM(InvalidStateError);      break;
    case WEBVIEW_ERROR_INVALID_ARGUMENT:   name = MRB_SYM(InvalidArgumentError);   break;
    case WEBVIEW_ERROR_DUPLICATE:          name = MRB_SYM(DuplicateError);         break;
    case WEBVIEW_ERROR_NOT_FOUND:          name = MRB_SYM(NotFoundError);          break;
    case WEBVIEW_ERROR_UNSPECIFIED:
    default:                               name = MRB_SYM(Error);                  break;
    }
    return mrb_class_get_under_id(mrb, base, name);
}

static void
hypha_check(mrb_state* mrb, webview_error_t code, const std::string& msg)
{
    if (code == WEBVIEW_ERROR_OK) return;
    const char* fallback;
    switch (code) {
    case WEBVIEW_ERROR_MISSING_DEPENDENCY: fallback = "missing dependency"; break;
    case WEBVIEW_ERROR_CANCELED:           fallback = "operation canceled"; break;
    case WEBVIEW_ERROR_INVALID_STATE:      fallback = "invalid state"; break;
    case WEBVIEW_ERROR_INVALID_ARGUMENT:   fallback = "invalid argument"; break;
    case WEBVIEW_ERROR_DUPLICATE:          fallback = "duplicate"; break;
    case WEBVIEW_ERROR_NOT_FOUND:          fallback = "not found"; break;
    case WEBVIEW_ERROR_UNSPECIFIED:
    default:                               fallback = "unspecified error"; break;
    }
    mrb_raise(mrb, hypha_error_class(mrb, code),
        msg.empty() ? fallback : msg.c_str());
}

template <typename R>
static void
hypha_check_result(mrb_state* mrb, const R& r)
{
    if (r.ok()) return;
    const auto& info = r.error();
    hypha_check(mrb, info.code(), info.message());
}

/* ========================================================================= */
/* Bind machinery                                                            */
/*                                                                           */
/* Procs are stored in a hidden iv table on the Hypha module: name_sym ->   */
/* Proc. webview::bind() takes a std::function lambda; the lambda we hand   */
/* it captures only the name_sym, looks up the Proc via the iv table on    */
/* every JS call. That keeps the Proc GC-rooted (the iv table holds it)    */
/* without webview having to know anything about mruby.                    */
/* ========================================================================= */

static mrb_value
bindings_hash(mrb_state* mrb)
{
    mrb_value hypha = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Hypha)));
    mrb_value h = mrb_iv_get(mrb, hypha, MRB_SYM(bindings));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, hypha, MRB_SYM(bindings), h);
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

/* Runs synchronously on the UI thread (== main): looks up the Proc in
 * the hidden bindings hash, parses JSON args, invokes proc, resolves
 * the JS-side promise. JSON parse / proc call / JSON dump errors are
 * mapped to resolve(id, 1, "{name:, message:, backtrace:}"). */
static void
invoke_bound_proc(mrb_state* mrb, mrb_sym name_sym, webview::webview* wv,
    const std::string& id, const std::string& req)
{
    int ai = mrb_gc_arena_save(mrb);

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
        mrb_value msg = mrb_funcall_id(mrb, parsed, MRB_SYM(message), 0);
        mrb_value backtrace = mrb_funcall_id(mrb, parsed, MRB_SYM(backtrace), 0);
        wv->resolve(id, 1,
            make_error_json_str(mrb, mrb_str_new_lit(mrb, "ParseError"),
                msg, backtrace));
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
        mrb_value msg = mrb_funcall_id(mrb, result, MRB_SYM(message), 0);
        mrb_value cls = mrb_funcall_id(mrb, result, MRB_SYM(class), 0);
        mrb_value name = mrb_funcall_id(mrb, cls, MRB_SYM(name), 0);
        mrb_value backtrace = mrb_funcall_id(mrb, result, MRB_SYM(backtrace), 0);
        if (!mrb_string_p(name)) name = mrb_str_new_lit(mrb, "Error");
        wv->resolve(id, 1, make_error_json_str(mrb, name, msg, backtrace));
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

/* hypha_bind_on_main — exposed to src/ via webview_internal.h (extern decl).
 * Registers (or replaces) a binding for name → proc. Called from the
 * dispatch lambda on main with proc reconstructed in g_main_mrb. */
void
hypha_bind_on_main(mrb_state* mrb, webview::webview* wv,
    mrb_sym name_sym, const std::string& name, mrb_value proc)
{
    mrb_value bh = bindings_hash(mrb);

    /* Re-bind: just swap the proc, no need to call wv->bind again
     * (which would return WEBVIEW_ERROR_DUPLICATE). */
    if (!mrb_nil_p(mrb_hash_get(mrb, bh, mrb_symbol_value(name_sym)))) {
        mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), proc);
        return;
    }

    /* Fresh registration: hand webview a lambda that captures name_sym
     * and the wv pointer. The hidden iv hash keeps the proc alive. */
    auto err = wv->bind(name,
        [name_sym, wv](std::string id, std::string req, void*) {
            mrb_state* m = g_main_mrb.load(std::memory_order_acquire);
            if (m) invoke_bound_proc(m, name_sym, wv, id, req);
        },
        nullptr);
    hypha_check_result(mrb, err);

    mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), proc);
}

void
hypha_unbind_on_main(mrb_state* mrb, webview::webview* wv, mrb_sym name_sym,
    const std::string& name)
{

    auto err = wv->unbind(name);
    hypha_check_result(mrb, err);

    mrb_hash_delete_key(mrb, bindings_hash(mrb), mrb_symbol_value(name_sym));
}

/* ========================================================================= */
/* set_size / return_result / bindings / handles                             */
/* ========================================================================= */

void
hypha_set_size_on_main(webview::webview* wv, int w, int h, int hint)
{
    wv->set_size(w, h, static_cast<webview_hint_t>(hint));
}

void
hypha_return_result_on_main(webview::webview* wv, const std::string& id,
    int status, const std::string& result)
{
    wv->resolve(id, status, result);
}

mrb_value
hypha_bindings_on_main(mrb_state* mrb)
{
    mrb_value bh = mrb_iv_get(mrb,
        mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Hypha))),
        MRB_SYM(bindings));
    if (!mrb_hash_p(bh)) return mrb_ary_new(mrb);

    mrb_value keys = mrb_hash_keys(mrb, bh);
    mrb_int len = RARRAY_LEN(keys);
    mrb_value result = mrb_ary_new_capa(mrb, len);
    for (mrb_int i = 0; i < len; i++) {
        mrb_ary_push(mrb, result, mrb_ary_ref(mrb, keys, i));
    }
    return result;
}

mrb_value
hypha_window_handle_on_main(mrb_state* mrb, webview::webview* wv)
{
    auto r = wv->window();
    if (!r.ok()) return mrb_nil_value();
    void* p = r.value();
    return p ? mrb_cptr_value(mrb, p) : mrb_nil_value();
}

mrb_value
hypha_native_handle_on_main(mrb_state* mrb, webview::webview* wv,
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
/*                                                                           */
/* Per-platform implementations. Userdata struct + callback; CDATA wrapper  */
/* class so the GC manages teardown. fds_hash is a hidden iv on Hypha that  */
/* maps fd_obj → _FDUD instance; remove_native_event uses it to find        */
/* watchers, GC walks it to keep _FDUDs alive while the watcher is live.   */
/* ========================================================================= */

static mrb_value
fds_hash(mrb_state* mrb)
{
    mrb_value hypha = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Hypha)));
    mrb_value h = mrb_iv_get(mrb, hypha, MRB_SYM(fds_procs));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, hypha, MRB_SYM(fds_procs), h);
    }
    return h;
}

#ifdef WEBVIEW_GTK
#include <glib-unix.h>

struct mrb_hypha_fd_ud {
    mrb_state* mrb;
    mrb_value fd;
    mrb_value blk;
    guint id = 0;

    ~mrb_hypha_fd_ud() {
        if (id) g_source_remove(id);
    }
};

MRB_CPP_DEFINE_TYPE(mrb_hypha_fd_ud, Hypha_FDUD);

static gboolean
on_fd_ready(gint /*fd*/, GIOCondition condition, gpointer user_data)
{
    auto* ud = static_cast<mrb_hypha_fd_ud*>(user_data);
    mrb_state* mrb = ud->mrb;
    int ai = mrb_gc_arena_save(mrb);

    const mrb_value argv[] = {
        ud->fd, mrb_int_value(mrb, condition)
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
hypha_fdud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value fd, blk;
    mrb_get_args(mrb, "oo", &fd, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(fd), fd);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_hypha_fd_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, self);
    ud->mrb = mrb;
    ud->fd = fd;
    ud->blk = blk;
    return self;
}

void
hypha_add_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj, mrb_value blk)
{
    int fd = (int)mrb_integer(mrb_type_convert(mrb, fd_obj,
        MRB_TT_INTEGER, MRB_SYM(fileno)));

    struct RClass* hypha = mrb_module_get_id(mrb, MRB_SYM(Hypha));
    struct RClass* fdud_cls = mrb_class_get_under_id(mrb, hypha, MRB_SYM(_FDUD));

    mrb_value argv[] = { fd_obj, blk };
    mrb_value ud_obj = mrb_obj_new(mrb, fdud_cls, 2, argv);
    mrb_hash_set(mrb, fds_hash(mrb), fd_obj, ud_obj);

    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, ud_obj);
    ud->id = g_unix_fd_add(fd, G_IO_IN, on_fd_ready, DATA_PTR(ud_obj));
}

void
hypha_remove_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj)
{
    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (mrb_undef_p(ud_obj)) return;
    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, ud_obj);
    g_source_remove(ud->id);
    ud->id = 0;
    mrb_hash_delete_key(mrb, fh, fd_obj);
}
#endif /* WEBVIEW_GTK */

#ifdef WEBVIEW_COCOA
#include <CoreFoundation/CoreFoundation.h>

struct mrb_hypha_fd_ud {
    mrb_state* mrb;
    mrb_value fd;
    mrb_value blk;
    CFFileDescriptorRef cf_fd = nullptr;
    CFRunLoopSourceRef  src = nullptr;

    ~mrb_hypha_fd_ud() {
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

MRB_CPP_DEFINE_TYPE(mrb_hypha_fd_ud, Hypha_FDUD);

static void
on_cf_fd_ready(CFFileDescriptorRef cf_fd, CFOptionFlags /*types*/, void* info)
{
    auto* ud = static_cast<mrb_hypha_fd_ud*>(info);
    mrb_state* mrb = ud->mrb;
    int ai = mrb_gc_arena_save(mrb);

    const mrb_value argv[] = { ud->fd, mrb_int_value(mrb, 1) /* G_IO_IN-equivalent */ };
    mrb_bool cont = mrb_test(mrb_yield_argv(mrb, ud->blk, 2, argv));

    if (cont) {
        CFFileDescriptorEnableCallBacks(cf_fd, kCFFileDescriptorReadCallBack);
    }
    else {
        mrb_hash_delete_key(mrb, fds_hash(mrb), ud->fd);
    }
    mrb_gc_arena_restore(mrb, ai);
}

static mrb_value
hypha_fdud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value fd, blk;
    mrb_get_args(mrb, "oo", &fd, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(fd), fd);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_hypha_fd_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, self);
    ud->mrb = mrb;
    ud->fd = fd;
    ud->blk = blk;
    return self;
}

void
hypha_add_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj, mrb_value blk)
{
    int fd = (int)mrb_integer(mrb_type_convert(mrb, fd_obj,
        MRB_TT_INTEGER, MRB_SYM(fileno)));

    struct RClass* hypha = mrb_module_get_id(mrb, MRB_SYM(Hypha));
    struct RClass* fdud_cls = mrb_class_get_under_id(mrb, hypha, MRB_SYM(_FDUD));

    mrb_value argv[] = { fd_obj, blk };
    mrb_value ud_obj = mrb_obj_new(mrb, fdud_cls, 2, argv);
    mrb_hash_set(mrb, fds_hash(mrb), fd_obj, ud_obj);

    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, ud_obj);

    CFFileDescriptorContext ctx = { 0, ud, nullptr, nullptr, nullptr };
    ud->cf_fd = CFFileDescriptorCreate(kCFAllocatorDefault, fd,
        /*closeOnInvalidate=*/false,
        on_cf_fd_ready, &ctx);
    if (!ud->cf_fd) {
        mrb_hash_delete_key(mrb, fds_hash(mrb), fd_obj);
        mrb_raise(mrb, E_RUNTIME_ERROR, "CFFileDescriptorCreate failed");
    }
    CFFileDescriptorEnableCallBacks(ud->cf_fd, kCFFileDescriptorReadCallBack);

    ud->src = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, ud->cf_fd, 0);
    if (!ud->src) {
        mrb_hash_delete_key(mrb, fds_hash(mrb), fd_obj);
        mrb_raise(mrb, E_RUNTIME_ERROR, "CFFileDescriptorCreateRunLoopSource failed");
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), ud->src, kCFRunLoopCommonModes);
}

void
hypha_remove_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj)
{
    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (!mrb_undef_p(ud_obj)) {
        mrb_hash_delete_key(mrb, fh, fd_obj);  /* dtor handles teardown */
    }
}
#endif /* WEBVIEW_COCOA */

#ifdef WEBVIEW_EDGE

#define MRB_HYPHA_WM_FD (WM_APP + 1)
static const wchar_t MRB_HYPHA_WND_CLASS[] = L"hypha_fd_dispatcher";

struct mrb_hypha_fd_ud {
    mrb_state* mrb;
    mrb_value  fd;
    mrb_value  blk;
    int        sock = -1;
    HWND       hwnd = nullptr;

    ~mrb_hypha_fd_ud() {
        if (sock != -1 && hwnd && IsWindow(hwnd)) {
            WSAAsyncSelect((SOCKET)sock, hwnd, 0, 0);
        }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_hypha_fd_ud, Hypha_FDUD);

struct mrb_hypha_wnd_ctx {
    mrb_state* mrb = nullptr;
    HWND       hwnd = nullptr;

    ~mrb_hypha_wnd_ctx() {
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

MRB_CPP_DEFINE_TYPE(mrb_hypha_wnd_ctx, Hypha_WndCtx);

static mrb_value
sockmap_hash(mrb_state* mrb)
{
    mrb_value hypha = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Hypha)));
    mrb_value h = mrb_iv_get(mrb, hypha, MRB_SYM(_native_event_sockmap));
    if (!mrb_hash_p(h)) {
        h = mrb_hash_new(mrb);
        mrb_iv_set(mrb, hypha, MRB_SYM(_native_event_sockmap), h);
    }
    return h;
}

static LRESULT CALLBACK
hypha_fd_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg != MRB_HYPHA_WM_FD) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* ctx = reinterpret_cast<mrb_hypha_wnd_ctx*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ctx) return 0;
    mrb_state* mrb = ctx->mrb;

    int sock = (int)wParam;
    int err = WSAGETSELECTERROR(lParam);
    int evt = WSAGETSELECTEVENT(lParam);

    int ai = mrb_gc_arena_save(mrb);

    mrb_value smap = sockmap_hash(mrb);
    mrb_value sk_v = mrb_int_value(mrb, sock);
    mrb_value fd_obj = mrb_hash_fetch(mrb, smap, sk_v, mrb_undef_value());
    if (mrb_undef_p(fd_obj)) { mrb_gc_arena_restore(mrb, ai); return 0; }

    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (mrb_undef_p(ud_obj)) { mrb_gc_arena_restore(mrb, ai); return 0; }

    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, ud_obj);
    if (!ud) { mrb_gc_arena_restore(mrb, ai); return 0; }

    const mrb_value argv[] = { ud->fd, mrb_int_value(mrb, evt) };
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
hypha_wnd_ctx_init(mrb_state* mrb, mrb_value self)
{
    mrb_cpp_new<mrb_hypha_wnd_ctx>(mrb, self);
    auto* ctx = mrb_cpp_get<mrb_hypha_wnd_ctx>(mrb, self);
    ctx->mrb = mrb;
    return self;
}

static HWND
get_or_create_hypha_wnd(mrb_state* mrb)
{
    mrb_value hypha = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Hypha)));
    mrb_value ctx_v = mrb_iv_get(mrb, hypha, MRB_SYM(_native_event_wnd_ctx));
    mrb_hypha_wnd_ctx* ctx = nullptr;
    if (mrb_data_p(ctx_v)) {
        ctx = mrb_cpp_get<mrb_hypha_wnd_ctx>(mrb, ctx_v);
        if (ctx && ctx->hwnd) return ctx->hwnd;
    }

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    if (!GetClassInfoExW(hinst, MRB_HYPHA_WND_CLASS, &wc)) {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = hypha_fd_wnd_proc;
        wc.hInstance = hinst;
        wc.lpszClassName = MRB_HYPHA_WND_CLASS;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            mrb_raisef(mrb, E_RUNTIME_ERROR,
                "RegisterClassEx failed: %d", (mrb_int)GetLastError());
        }
    }

    if (!ctx) {
        struct RClass* hypha_cls = mrb_module_get_id(mrb, MRB_SYM(Hypha));
        struct RClass* ctx_cls = mrb_class_get_under_id(mrb, hypha_cls, MRB_SYM(_WndCtx));
        ctx_v = mrb_obj_new(mrb, ctx_cls, 0, nullptr);
        mrb_iv_set(mrb, hypha, MRB_SYM(_native_event_wnd_ctx), ctx_v);
        ctx = mrb_cpp_get<mrb_hypha_wnd_ctx>(mrb, ctx_v);
    }

    HWND hwnd = CreateWindowExW(0, MRB_HYPHA_WND_CLASS, L"",
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
hypha_fdud_init(mrb_state* mrb, mrb_value self)
{
    mrb_value fd, blk;
    mrb_get_args(mrb, "oo", &fd, &blk);
    mrb_iv_set(mrb, self, MRB_SYM(fd), fd);
    mrb_iv_set(mrb, self, MRB_SYM(blk), blk);
    mrb_cpp_new<mrb_hypha_fd_ud>(mrb, self);
    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, self);
    ud->mrb = mrb;
    ud->fd = fd;
    ud->blk = blk;
    return self;
}

void
hypha_add_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj, mrb_value blk)
{
    int fd = (int)mrb_integer(mrb_type_convert(mrb, fd_obj,
        MRB_TT_INTEGER, MRB_SYM(fileno)));

    int sotype = 0;
    int solen = sizeof(sotype);
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE,
        reinterpret_cast<char*>(&sotype), &solen) == SOCKET_ERROR) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "Hypha.add_native_event on Windows requires a winsock SOCKET "
            "(WSAGetLastError=%d)", (mrb_int)WSAGetLastError());
    }

    HWND hwnd = get_or_create_hypha_wnd(mrb);

    struct RClass* hypha = mrb_module_get_id(mrb, MRB_SYM(Hypha));
    struct RClass* fdud_cls = mrb_class_get_under_id(mrb, hypha, MRB_SYM(_FDUD));
    mrb_value argv[] = { fd_obj, blk };
    mrb_value ud_obj = mrb_obj_new(mrb, fdud_cls, 2, argv);
    mrb_hash_set(mrb, fds_hash(mrb), fd_obj, ud_obj);

    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, ud_obj);
    ud->sock = fd;
    ud->hwnd = hwnd;

    mrb_hash_set(mrb, sockmap_hash(mrb), mrb_int_value(mrb, fd), fd_obj);

    long mask = FD_READ | FD_ACCEPT | FD_CLOSE | FD_OOB;
    if (WSAAsyncSelect((SOCKET)fd, hwnd, MRB_HYPHA_WM_FD, mask) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        mrb_hash_delete_key(mrb, sockmap_hash(mrb), mrb_int_value(mrb, fd));
        mrb_hash_delete_key(mrb, fds_hash(mrb), fd_obj);
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "WSAAsyncSelect failed: %d", (mrb_int)err);
    }
}

void
hypha_remove_native_event_on_main(mrb_state* mrb, webview::webview* /*wv*/,
    mrb_value fd_obj)
{
    mrb_value fh = fds_hash(mrb);
    mrb_value ud_obj = mrb_hash_fetch(mrb, fh, fd_obj, mrb_undef_value());
    if (mrb_undef_p(ud_obj)) return;

    auto* ud = mrb_cpp_get<mrb_hypha_fd_ud>(mrb, ud_obj);
    if (ud && ud->sock != -1 && ud->hwnd) {
        WSAAsyncSelect((SOCKET)ud->sock, ud->hwnd, 0, 0);
        mrb_hash_delete_key(mrb, sockmap_hash(mrb), mrb_int_value(mrb, ud->sock));
        ud->sock = -1;
    }
    mrb_hash_delete_key(mrb, fh, fd_obj);
}
#endif /* WEBVIEW_EDGE */

/* ========================================================================= */
/* Win32 default menu and icon                                               */
/* ========================================================================= */

#ifdef WEBVIEW_EDGE

enum {
    MRB_HYPHA_MENU_ID_BASE = 0xA000,
    MRB_HYPHA_MENU_ID_FILE_NEW = MRB_HYPHA_MENU_ID_BASE,
    MRB_HYPHA_MENU_ID_FILE_OPEN,
    MRB_HYPHA_MENU_ID_FILE_SAVE,
    MRB_HYPHA_MENU_ID_FILE_QUIT,
    MRB_HYPHA_MENU_ID_EDIT_UNDO,
    MRB_HYPHA_MENU_ID_EDIT_REDO,
    MRB_HYPHA_MENU_ID_HELP_ABOUT,
};

static HMENU
build_default_menu()
{
    HMENU bar = CreateMenu();
    if (!bar) return nullptr;

    HMENU file_menu = CreatePopupMenu();
    AppendMenuA(file_menu, MF_STRING, MRB_HYPHA_MENU_ID_FILE_NEW, "&New\tCtrl+N");
    AppendMenuA(file_menu, MF_STRING, MRB_HYPHA_MENU_ID_FILE_OPEN, "&Open…\tCtrl+O");
    AppendMenuA(file_menu, MF_STRING, MRB_HYPHA_MENU_ID_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(file_menu, MF_STRING, MRB_HYPHA_MENU_ID_FILE_QUIT, "&Quit\tAlt+F4");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file_menu, "&File");

    HMENU edit_menu = CreatePopupMenu();
    AppendMenuA(edit_menu, MF_STRING, MRB_HYPHA_MENU_ID_EDIT_UNDO, "&Undo\tCtrl+Z");
    AppendMenuA(edit_menu, MF_STRING, MRB_HYPHA_MENU_ID_EDIT_REDO, "&Redo\tCtrl+Y");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)edit_menu, "&Edit");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuA(help_menu, MF_STRING, MRB_HYPHA_MENU_ID_HELP_ABOUT, "&About");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help_menu, "&Help");

    return bar;
}

static LRESULT CALLBACK
hypha_menu_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*idSubclass*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_COMMAND && HIWORD(wParam) == 0) {
        UINT id = LOWORD(wParam);
        if (id >= MRB_HYPHA_MENU_ID_BASE && id <= MRB_HYPHA_MENU_ID_HELP_ABOUT) {
            switch (id) {
            case MRB_HYPHA_MENU_ID_FILE_QUIT:
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            default:
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void
attach_menu_and_icon(webview::webview* wv)
{
    auto window_result = wv->window();
    if (!window_result.ok()) return;
    HWND hwnd = static_cast<HWND>(window_result.value());
    if (!hwnd || !IsWindow(hwnd)) return;

    /* Window icon — same ICON resource the .exe uses (id 1, baked in
     * via mruby-webview.rc). */
    HINSTANCE hinst = GetModuleHandleA(nullptr);
    HICON small_icon = (HICON)LoadImageA(hinst, MAKEINTRESOURCEA(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), 0);
    HICON big_icon = (HICON)LoadImageA(hinst, MAKEINTRESOURCEA(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON), 0);
    if (small_icon) SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    if (big_icon)   SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big_icon);

    /* Default menu bar */
    HMENU bar = build_default_menu();
    if (!bar) return;
    if (!SetMenu(hwnd, bar)) {
        DestroyMenu(bar);
        return;
    }

    /* Force WM_NCCALCSIZE so webview recomputes its WebView2 controller
     * bounds against the new (menu-shrunk) client area. */
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    /* Subclass to intercept WM_COMMAND for our menu items. */
    SetWindowSubclass(hwnd, hypha_menu_subclass_proc, 1, 0);
}

#else
static void attach_menu_and_icon(webview::webview*) {}
#endif

/* ========================================================================= */
/* Hypha.run and Hypha.ready                                                 */
/* ========================================================================= */

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

/* Hypha.ready { ... } — register the one-shot hook. Setting a second
 * time replaces the first; clearing happens automatically when fired. */
static mrb_value
mrb_hypha_ready(mrb_state* mrb, mrb_value /*self*/)
{
    mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, "&!", &blk);
    if (mrb_nil_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "Hypha.ready requires a block");
    }
    if (g_ready_hook_set) {
        /* unregister the previous one to allow replacement; rare but cheap */
        mrb_gc_unregister(mrb, g_ready_hook);
        g_ready_hook_set = false;
    }
    mrb_gc_register(mrb, blk);
    g_ready_hook = blk;
    g_ready_hook_set = true;
    return mrb_nil_value();
}

/* Hypha.run(title:, size:, debug:, html:, url:, init:) { |hypha| ... }     */
static mrb_value
mrb_hypha_run(mrb_state* mrb, mrb_value self)
{
    if (g_hypha_used) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
            "Hypha.run can only be called once per process");
    }
    if (!hypha_in_main_state(mrb)) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
            "Hypha.run must be called from the main thread mruby vm");
    }
    g_hypha_used = true;

    /* Parse kwargs. We accept all of these as optional keyword arguments. */
    mrb_value kw_title = mrb_nil_value();
    mrb_value kw_size = mrb_nil_value();
    mrb_bool  kw_debug = FALSE;
    mrb_value kw_html = mrb_nil_value();
    mrb_value kw_url = mrb_nil_value();
    mrb_value kw_init = mrb_nil_value();

    const mrb_sym kw_names[] = {
        MRB_SYM(title), MRB_SYM(size), MRB_SYM(debug),
        MRB_SYM(html),  MRB_SYM(url),  MRB_SYM(init)
    };
    mrb_value kw_values[6];
    mrb_kwargs kwargs = {
        6, 0, kw_names, kw_values, nullptr
    };
    mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, ":&", &kwargs, &blk);

    if (!mrb_undef_p(kw_values[0])) kw_title = kw_values[0];
    if (!mrb_undef_p(kw_values[1])) kw_size = kw_values[1];
    if (!mrb_undef_p(kw_values[2])) kw_debug = mrb_test(kw_values[2]);
    if (!mrb_undef_p(kw_values[3])) kw_html = kw_values[3];
    if (!mrb_undef_p(kw_values[4])) kw_url = kw_values[4];
    if (!mrb_undef_p(kw_values[5])) kw_init = kw_values[5];

    if (!mrb_nil_p(kw_html) && !mrb_nil_p(kw_url)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR,
            "Hypha.run accepts either html: or url:, not both");
    }

    /* Construct the webview. Constructor args (debug + parent window)
     * are baked in here; we don't expose a window: kwarg for now. */
    webview::webview* wv;
    try {
        wv = new webview::webview(static_cast<bool>(kw_debug), nullptr);
    }
    catch (const webview::exception& e) {
        hypha_check(mrb, e.error().code(), e.error().message());
        return mrb_nil_value();   /* unreachable */
    }

    /* Publish g_wv before any helper that might dispatch can run.
     * Release ordering pairs with workers' acquire-loads. */
    g_wv.store(wv, std::memory_order_release);

    /* Apply kwargs synchronously (we're on main, no dispatch needed). */
    if (mrb_string_p(kw_title)) {
        wv->set_title(to_std_string(kw_title));
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
        wv->set_size((int)w, (int)h, hint);
    }
    if (mrb_string_p(kw_init)) {
        wv->init(to_std_string(kw_init));
    }
    if (mrb_string_p(kw_html)) {
        wv->set_html(to_std_string(kw_html));
    }
    else if (mrb_string_p(kw_url)) {
        wv->navigate(to_std_string(kw_url));
    }

    /* Win32: attach default menu and icon now that the HWND is real. */
    attach_menu_and_icon(wv);

    /* Yield the Hypha module itself to the user's setup block. The user
     * can call h.bind, h.html=, etc. — same methods as Hypha.bind etc.,
     * fast-pathed since we're on main. */
    if (mrb_proc_p(blk)) {
        mrb_value hypha_module = mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(Hypha)));
        struct ctx { mrb_value blk; mrb_value arg; };
        ctx c{ blk, hypha_module };

        mrb_bool err = FALSE;
        mrb_protect_error(mrb,
            [](mrb_state* m, void* p) -> mrb_value {
                ctx* c = static_cast<ctx*>(p);
                return mrb_yield_argv(m, c->blk, 1, &c->arg);
            },
            &c, &err);

        if (err) {
            /* Setup-block error: tear down webview and re-raise so the
             * user sees the real error without the run loop ever starting. */
            mrb_value exc = mrb_obj_value(mrb->exc);
            mrb->exc = nullptr;
            g_wv.store(nullptr, std::memory_order_release);
            delete wv;
            mrb_exc_raise(mrb, exc);
            return mrb_nil_value();   /* unreachable */
        }
    }

    /* Fire the ready hook (if any) before run() begins pumping. */
    if (g_ready_hook_set) {
        mrb_value hook = g_ready_hook;
        mrb_gc_unregister(mrb, hook);
        g_ready_hook_set = false;

        struct ctx { mrb_value blk; };
        ctx c{ hook };

        mrb_bool err = FALSE;
        mrb_protect_error(mrb,
            [](mrb_state* m, void* p) -> mrb_value {
                ctx* c = static_cast<ctx*>(p);
                return mrb_yield_argv(m, c->blk, 0, nullptr);
            },
            &c, &err);

        if (err) {
            mrb_value exc = mrb_obj_value(mrb->exc);
            mrb->exc = nullptr;
            g_wv.store(nullptr, std::memory_order_release);
            delete wv;
            mrb_exc_raise(mrb, exc);
            return mrb_nil_value();   /* unreachable */
        }
    }

    /* Block here until terminate() is called or the window closes. */
    auto run_result = wv->run();

    /* Shutdown: clear g_wv (release) before destroying so any in-flight
     * worker dispatches see null and bail out. webview's destructor
     * drains the dispatch queue first, so already-queued lambdas still
     * run on main with g_wv still set — safe. */
    g_wv.store(nullptr, std::memory_order_release);
    delete wv;

    hypha_check_result(mrb, run_result);
    return self;
}

/* ========================================================================= */
/* Main-only Hypha.* methods.                                                */
/*                                                                           */
/* These touch state that can't cross thread boundaries (procs holding their*/
/* mrb_state's lexical environment, mrb_state-resident hash entries, native */
/* handles meaningful only on the UI thread). Defined here in tools/hypha/  */
/* since they need access to platform-specific helpers (bind dispatcher,    */
/* fd-watcher CDatas) and the live webview's internal state.                */
/*                                                                           */
/* All raise from non-main callers (workers should use Hypha.dispatch to    */
/* run such code on main).                                                  */
/* ========================================================================= */

static webview::webview*
hypha_require_running_local(mrb_state* mrb)
{
    webview::webview* wv = g_wv.load(std::memory_order_acquire);
    if (!wv) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "Hypha is not running");
    }
    return wv;
}

static void
hypha_require_main_state(mrb_state* mrb, const char* method_name)
{
    if (!hypha_in_main_state(mrb)) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
            "%s can only be called from the main thread mruby vm; "
            "use Hypha.dispatch to run code on main from a worker",
            method_name);
    }
}

/* Hypha.bind(name, &blk) -- main only. */
static mrb_value
mrb_hypha_bind(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.bind");

    mrb_sym name_sym;
    mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, "n&", &name_sym, &blk);
    if (mrb_nil_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "Hypha.bind requires a block");
    }

    mrb_int name_len;
    const char* name_s = mrb_sym_name_len(mrb, name_sym, &name_len);
    std::string name(name_s, static_cast<size_t>(name_len));

    webview::webview* wv = hypha_require_running_local(mrb);
    hypha_bind_on_main(mrb, wv, name_sym, name, blk);
    return mrb_nil_value();
}

/* Hypha.unbind(name) -- main only. */
static mrb_value
mrb_hypha_unbind(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.unbind");

    mrb_sym name_sym;
    mrb_get_args(mrb, "n", &name_sym);

    mrb_int name_len;
    const char* name_s = mrb_sym_name_len(mrb, name_sym, &name_len);
    std::string name(name_s, static_cast<size_t>(name_len));

    webview::webview* wv = hypha_require_running_local(mrb);
    hypha_unbind_on_main(mrb, wv, name_sym, name);
    return mrb_nil_value();
}

/* Hypha.add_native_event(io, &blk) -- main only. */
static mrb_value
mrb_hypha_add_native_event(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.add_native_event");

    mrb_value fd_obj;
    mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, "o&", &fd_obj, &blk);
    if (mrb_nil_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR,
            "Hypha.add_native_event requires a block");
    }

    webview::webview* wv = hypha_require_running_local(mrb);
    hypha_add_native_event_on_main(mrb, wv, fd_obj, blk);
    return mrb_nil_value();
}

/* Hypha.remove_native_event(io) -- main only. */
static mrb_value
mrb_hypha_remove_native_event(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.remove_native_event");

    mrb_value fd_obj;
    mrb_get_args(mrb, "o", &fd_obj);

    webview::webview* wv = hypha_require_running_local(mrb);
    hypha_remove_native_event_on_main(mrb, wv, fd_obj);
    return mrb_nil_value();
}

/* Hypha.bindings -- Array<Symbol> of registered binding names. Main only. */
static mrb_value
mrb_hypha_bindings(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.bindings");
    /* No webview required; the iv just may-be-empty before run starts. */
    return hypha_bindings_on_main(mrb);
}

/* Hypha.window_handle -- mrb_cptr or nil. Main only. */
static mrb_value
mrb_hypha_window_handle(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.window_handle");
    webview::webview* wv = hypha_require_running_local(mrb);
    return hypha_window_handle_on_main(mrb, wv);
}

/* Hypha.handle(kind=:window) -- :window | :widget | :browser_controller. */
static mrb_value
mrb_hypha_handle(mrb_state* mrb, mrb_value /*self*/)
{
    hypha_require_main_state(mrb, "Hypha.handle");
    mrb_value kind = mrb_symbol_value(MRB_SYM(window));
    mrb_get_args(mrb, "|o", &kind);
    webview::webview* wv = hypha_require_running_local(mrb);
    return hypha_native_handle_on_main(mrb, wv, kind);
}

/* ========================================================================= */
/* Gem extension: register Hypha.run, Hypha.ready, error classes, and the   */
/* internal CData helper classes (_FDUD, _WndCtx).                          */
/*                                                                           */
/* This runs after src/hypha_methods.cpp's gem_init has defined the Hypha   */
/* module and its methods. We just add to it.                                */
/* ========================================================================= */

static void
hypha_install_runtime(mrb_state* mrb)
{
    struct RClass* hypha = mrb_module_get_id(mrb, MRB_SYM(Hypha));

    /* Error class hierarchy. */
    struct RClass* err = mrb_define_class_under_id(mrb, hypha,
        MRB_SYM(Error),
        E_RUNTIME_ERROR);
    mrb_define_class_under_id(mrb, hypha, MRB_SYM(MissingDependencyError), err);
    mrb_define_class_under_id(mrb, hypha, MRB_SYM(CanceledError), err);
    mrb_define_class_under_id(mrb, hypha, MRB_SYM(InvalidStateError), err);
    mrb_define_class_under_id(mrb, hypha, MRB_SYM(InvalidArgumentError), err);
    mrb_define_class_under_id(mrb, hypha, MRB_SYM(DuplicateError), err);
    mrb_define_class_under_id(mrb, hypha, MRB_SYM(NotFoundError), err);

    /* Hypha._FDUD — internal CData wrapping platform fd-watcher state. */
    struct RClass* fdud_cls = mrb_define_class_under_id(mrb, hypha,
        MRB_SYM(_FDUD),
        mrb->object_class);
    MRB_SET_INSTANCE_TT(fdud_cls, MRB_TT_CDATA);
    mrb_define_method_id(mrb, fdud_cls, MRB_SYM(initialize),
        hypha_fdud_init, MRB_ARGS_REQ(2));

#ifdef WEBVIEW_EDGE
    /* Hypha._WndCtx — Windows-only, owns the hidden HWND for fd dispatch. */
    struct RClass* ctx_cls = mrb_define_class_under_id(mrb, hypha,
        MRB_SYM(_WndCtx),
        mrb->object_class);
    MRB_SET_INSTANCE_TT(ctx_cls, MRB_TT_CDATA);
    mrb_define_method_id(mrb, ctx_cls, MRB_SYM(initialize),
        hypha_wnd_ctx_init, MRB_ARGS_NONE());
#endif

    /* Lifecycle entry points. */
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(run),
        mrb_hypha_run,
        MRB_ARGS_KEY(6, 0) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(ready),
        mrb_hypha_ready, MRB_ARGS_BLOCK());

    /* Main-only methods that touch mrb_state-resident state. */
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(bind),
        mrb_hypha_bind,
        MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(unbind),
        mrb_hypha_unbind, MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(add_native_event),
        mrb_hypha_add_native_event,
        MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(remove_native_event),
        mrb_hypha_remove_native_event,
        MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(bindings),
        mrb_hypha_bindings, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(window_handle),
        mrb_hypha_window_handle, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(handle),
        mrb_hypha_handle, MRB_ARGS_OPT(1));
}

/* ========================================================================= */
/* main()                                                                    */
/* ========================================================================= */

int
main(int argc, const char* argv[])
{
    mrb_state* mrb = mrb_open();
    if (!mrb) return 1;
    g_main_mrb.store(mrb, std::memory_order_release);

    int exit_code = 0;

    /* Expose ARGV. Frozen so users can't mutate the original argv block. */
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

    /* Register Hypha.run, Hypha.ready, error classes, _FDUD/_WndCtx.
     * src/hypha_methods.cpp's gem_init has already defined the rest. */
    hypha_install_runtime(mrb);

    /* Run the embedded user script. It typically calls Hypha.run, which
     * blocks until the window closes. */
    mrb_load_irep(mrb, hypha_main);
    if (mrb->exc) {
        mrb_print_error(mrb);
        exit_code = 1;
    }

    g_main_mrb.store(nullptr, std::memory_order_release);
    mrb_close(mrb);
    return exit_code;
}