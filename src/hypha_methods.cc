/*
 * hypha_methods.cpp — Hypha module methods that don't need platform code.
 *
 * What lives here:
 *   - The two globals that pin the architecture: g_main_mrb, g_wv.
 *     Definitions are here so they sit in libmruby.lib; other binaries
 *     that link libmruby.lib see harmless null atomics, and the hypha
 *     binary's tools/hypha/hypha.cpp reads/writes them via the externs
 *     in webview_internal.h.
 *
 *   - Methods that act on the live webview without any platform-specific
 *     work: title=, html=, url=, eval, init, terminate, set_size,
 *     resolve. Each follows the same pattern: extract args, gate
 *     on g_wv, branch on hypha_in_main_state(mrb) — direct call on main,
 *     webview::dispatch(lambda) off main. Lambdas capture only plain
 *     C++ data (std::string, int, bool); never mrb_value, never pointers
 *     into the caller's mrb_state.
 *
 *   - Hypha.dispatch: the cross-thread proc carrier. The proc is dumped
 *     via mruby-proc-irep-ext (#to_irep_bytecode), args are CBOR-encoded
 *     as an array, both captured as std::string into the dispatch lambda.
 *     On main, the lambda decodes against g_main_mrb and yields. The
 *     dispatched proc must be self-contained: references to outer-scope
 *     locals or worker-only top-level methods will fail at first
 *     invocation with a NoMethodError, which is debuggable.
 *
 *   - Always-safe methods: version (static webview C call), platform
 *     (build-time constant), running? (atomic load).
 *
 * What lives in tools/hypha/hypha.cpp instead:
 *   - bind / unbind: the Proc holds onto its mrb_state's environment for
 *     its lifetime. Setup-time on main, no cross-thread story.
 *   - add_native_event / remove_native_event: platform-specific fd
 *     watchers (GTK / CFFileDescriptor / WSAAsyncSelect+HWND), main-only.
 *   - bindings, window_handle, handle: read state that lives only in
 *     main's mrb_state. Main-only.
 *   - Hypha.run, Hypha.ready: the lifecycle entry points.
 */

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/dump.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/numeric.h>
#include <mruby/presym.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <mruby/cbor.h>

#include <webview/webview.h>

#include <atomic>
#include <string>
#include <utility>

#include "webview_internal.h"

 /* ========================================================================= */
 /* Globals.                                                                  */
 /*                                                                           */
 /* Defined here so they sit in libmruby.lib. Other binaries that link        */
 /* libmruby.lib see them as null atomics, and every Hypha.* call routes     */
 /* to the "Hypha is not running" error path. The hypha binary stores into  */
 /* them from tools/hypha/hypha.cpp's main() and Hypha.run.                 */
 /* ========================================================================= */

std::atomic<mrb_state*>        g_main_mrb{ nullptr };
std::atomic<webview::webview*> g_wv{ nullptr };

/* ========================================================================= */
/* Gate                                                                      */
/* ========================================================================= */

static webview::webview*
hypha_require_running(mrb_state* mrb)
{
    webview::webview* wv = g_wv.load(std::memory_order_acquire);
    if (!wv) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
            "Hypha is not running (Hypha.run hasn't been called, "
            "or the run loop has already exited)");
    }
    return wv;
}

/* ========================================================================= */
/* CBOR-based proc / value serialization for Hypha.dispatch.                */
/* ========================================================================= */
static std::string
serialize_value_to_cbor(mrb_state* mrb, mrb_value v)
{
    mrb_value cbor = mrb_cbor_encode_fast(mrb, v);
    return std::string(RSTRING_PTR(cbor),
        static_cast<size_t>(RSTRING_LEN(cbor)));
}

static mrb_value
deserialize_value_from_cbor(mrb_state* mrb, const std::string& payload)
{
    mrb_value cbor_str = mrb_str_new(mrb, payload.data(), payload.size());
    return mrb_cbor_decode_fast(mrb, cbor_str);
}

static std::string
serialize_proc_to_cbor(mrb_state* mrb, mrb_value proc)
{
    mrb_value irep_bytes = mrb_cbor_encode_fast(mrb, proc);
    return std::string(RSTRING_PTR(irep_bytes),
        static_cast<size_t>(RSTRING_LEN(irep_bytes)));
}

static mrb_value
deserialize_proc_from_cbor(mrb_state* mrb, const std::string& payload)
{
	return mrb_cbor_decode_fast(mrb, mrb_str_new(mrb, payload.data(), payload.size()));
}

/* ========================================================================= */
/* Hypha module methods                                                      */
/* ========================================================================= */

/* Hypha.title=(s) ---------------------------------------------------------- */
static mrb_value
mrb_hypha_set_title(mrb_state* mrb, mrb_value /*self*/)
{
    const char* s; mrb_int len;
    mrb_get_args(mrb, "s", &s, &len);
    std::string title(s, static_cast<size_t>(len));

    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        wv->set_title(title);
    }
    else {
        wv->dispatch([title = std::move(title)]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->set_title(title);
            });
    }
    return mrb_nil_value();
}

/* Hypha.html=(s) ----------------------------------------------------------- */
static mrb_value
mrb_hypha_set_html(mrb_state* mrb, mrb_value /*self*/)
{
    const char* s; mrb_int len;
    mrb_get_args(mrb, "s", &s, &len);
    std::string html(s, static_cast<size_t>(len));

    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        wv->set_html(html);
    }
    else {
        wv->dispatch([html = std::move(html)]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->set_html(html);
            });
    }
    return mrb_nil_value();
}

/* Hypha.url=(s) / Hypha.navigate(s) --------------------------------------- */
static mrb_value
mrb_hypha_navigate(mrb_state* mrb, mrb_value /*self*/)
{
    const char* s; mrb_int len;
    mrb_get_args(mrb, "s", &s, &len);
    std::string url(s, static_cast<size_t>(len));

    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        wv->navigate(url);
    }
    else {
        wv->dispatch([url = std::move(url)]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->navigate(url);
            });
    }
    return mrb_nil_value();
}

/* Hypha.eval(js) ----------------------------------------------------------- */
static mrb_value
mrb_hypha_eval(mrb_state* mrb, mrb_value /*self*/)
{
    const char* s; mrb_int len;
    mrb_get_args(mrb, "s", &s, &len);
    std::string js(s, static_cast<size_t>(len));

    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        wv->eval(js);
    }
    else {
        wv->dispatch([js = std::move(js)]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->eval(js);
            });
    }
    return mrb_nil_value();
}

/* Hypha.init(js) — JS to run on every page load --------------------------- */
static mrb_value
mrb_hypha_init(mrb_state* mrb, mrb_value /*self*/)
{
    const char* s; mrb_int len;
    mrb_get_args(mrb, "s", &s, &len);
    std::string js(s, static_cast<size_t>(len));

    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        wv->init(js);
    }
    else {
        wv->dispatch([js = std::move(js)]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->init(js);
            });
    }
    return mrb_nil_value();
}

/* size hint Symbol → webview_hint_t int ----------------------------------- */
static int
hypha_size_hint_from_sym(mrb_state* mrb, mrb_value v)
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
    return WEBVIEW_HINT_NONE;   /* unreachable */
}

/* Hypha.set_size(w, h, hint=:none) ---------------------------------------- */
static mrb_value
mrb_hypha_set_size(mrb_state* mrb, mrb_value /*self*/)
{
    mrb_int w, h;
    mrb_value hint_v = mrb_nil_value();
    mrb_get_args(mrb, "ii|o!", &w, &h, &hint_v);
    int hint = hypha_size_hint_from_sym(mrb, hint_v);

    webview::webview* wv = hypha_require_running(mrb);

    int wi = static_cast<int>(w);
    int hi = static_cast<int>(h);

    if (hypha_in_main_state(mrb)) {
        wv->set_size(wi, hi, static_cast<webview_hint_t>(hint));
    }
    else {
        wv->dispatch([wi, hi, hint]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->set_size(wi, hi, static_cast<webview_hint_t>(hint));
            });
    }
    return mrb_nil_value();
}

/* Hypha.size=([w, h, hint]) ---------------------------------------------- */
static mrb_value
mrb_hypha_size_setter(mrb_state* mrb, mrb_value self)
{
    mrb_value ary;
    mrb_get_args(mrb, "A", &ary);
    mrb_int n = RARRAY_LEN(ary);
    if (n < 2 || n > 3) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR,
            "Hypha.size= expects [w, h] or [w, h, hint], got %i elements",
            n);
    }
    mrb_value w_v = mrb_ary_ref(mrb, ary, 0);
    mrb_value h_v = mrb_ary_ref(mrb, ary, 1);
    mrb_value hint_v = (n == 3) ? mrb_ary_ref(mrb, ary, 2) : mrb_nil_value();

    mrb_int w = mrb_integer(mrb_to_int(mrb, w_v));
    mrb_int h = mrb_integer(mrb_to_int(mrb, h_v));
    int hint = hypha_size_hint_from_sym(mrb, hint_v);

    webview::webview* wv = hypha_require_running(mrb);

    int wi = static_cast<int>(w);
    int hi = static_cast<int>(h);

    if (hypha_in_main_state(mrb)) {
        wv->set_size(wi, hi, static_cast<webview_hint_t>(hint));
    }
    else {
        wv->dispatch([wi, hi, hint]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->set_size(wi, hi, static_cast<webview_hint_t>(hint));
            });
    }
    return self;
}

/* Hypha.terminate --------------------------------------------------------- */
static mrb_value
mrb_hypha_terminate(mrb_state* mrb, mrb_value /*self*/)
{
    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        wv->terminate();
    }
    else {
        wv->dispatch([]() {
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (w) w->terminate();
            });
    }
    return mrb_nil_value();
}

/* Hypha.resolve(id, status, result) --------------------------------- */
static mrb_value
mrb_hypha_resolve(mrb_state* mrb, mrb_value /*self*/)
{
    const char* id; mrb_int id_len;
    mrb_int status;
    const char* result; mrb_int result_len;
    mrb_get_args(mrb, "sis", &id, &id_len, &status, &result, &result_len);

    std::string id_s(id, static_cast<size_t>(id_len));
    std::string res_s(result, static_cast<size_t>(result_len));
    int st = static_cast<int>(status);

    webview::webview* wv = hypha_require_running(mrb);

    wv->resolve(id_s, st, res_s);
    return mrb_nil_value();
}

/* Hypha.dispatch(*args, &blk) — fire-and-forget proc dispatch onto main.  */
static mrb_value
mrb_hypha_dispatch(mrb_state* mrb, mrb_value /*self*/)
{
    mrb_value* argv;
    mrb_int argc;
    mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, "*&", &argv, &argc, &blk);
    if (mrb_nil_p(blk)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "Hypha.dispatch requires a block");
    }

    webview::webview* wv = hypha_require_running(mrb);

    if (hypha_in_main_state(mrb)) {
        /* fast path: yield directly with mrb_protect_error */

        struct ctx { mrb_value blk; mrb_value* argv; mrb_int argc; };
        ctx c{ blk, argv, argc };

        mrb_bool err = FALSE;
        mrb_protect_error(mrb,
            [](mrb_state* m, void* p) -> mrb_value {
                ctx* c = static_cast<ctx*>(p);
                return mrb_yield_argv(m, c->blk, c->argc, c->argv);
            },
            &c, &err);

        if (err) {
            mrb_print_error(mrb);
            mrb->exc = nullptr;
        }
        return mrb_nil_value();
    }

    /* off-main: serialize args + proc, dispatch a lambda that
     * reconstructs and yields on main. */
    mrb_value args_ary = mrb_ary_new_from_values(mrb, argc, argv);
    std::string args_bytes = serialize_value_to_cbor(mrb, args_ary);
    std::string proc_bytes = serialize_proc_to_cbor(mrb, blk);

    wv->dispatch([args_bytes = std::move(args_bytes),
        proc_bytes = std::move(proc_bytes)]() {
            mrb_state* m = g_main_mrb.load(std::memory_order_acquire);
            webview::webview* w = g_wv.load(std::memory_order_acquire);
            if (!m || !w) return;

            int ai = mrb_gc_arena_save(m);

            mrb_value proc = deserialize_proc_from_cbor(m, proc_bytes);
            if (m->exc) {
                mrb_print_error(m); m->exc = nullptr;
                mrb_gc_arena_restore(m, ai);
                return;
            }
            mrb_value args = deserialize_value_from_cbor(m, args_bytes);
            if (m->exc) {
                mrb_print_error(m); m->exc = nullptr;
                mrb_gc_arena_restore(m, ai);
                return;
            }
            if (!mrb_array_p(args)) {
                mrb_gc_arena_restore(m, ai);
                return;
            }

            struct ctx { mrb_value proc; mrb_value args; };
            ctx c{ proc, args };

            mrb_bool err = FALSE;
            mrb_protect_error(m,
                [](mrb_state* mm, void* p) -> mrb_value {
                    ctx* c = static_cast<ctx*>(p);
                    return mrb_yield_argv(mm, c->proc,
                        RARRAY_LEN(c->args),
                        RARRAY_PTR(c->args));
                },
                &c, &err);

            if (err) {
                mrb_print_error(m);
                m->exc = nullptr;
            }
            mrb_gc_arena_restore(m, ai);
        });

    return mrb_nil_value();
}

/* Hypha.version — webview version info Hash. ----------------------------- */
static mrb_value
mrb_hypha_version(mrb_state* mrb, mrb_value /*self*/)
{
    const webview_version_info_t* info = webview_version();
    mrb_value h = mrb_hash_new_capa(mrb, 6);
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(major)),
        mrb_int_value(mrb, info->version.major));
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(minor)),
        mrb_int_value(mrb, info->version.minor));
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(patch)),
        mrb_int_value(mrb, info->version.patch));
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(version)),
        mrb_str_new_cstr(mrb, info->version_number));
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(pre_release)),
        mrb_str_new_cstr(mrb, info->pre_release));
    mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(build_metadata)),
        mrb_str_new_cstr(mrb, info->build_metadata));
    return h;
}

/* Hypha.platform — :macos | :windows | :linux | :unknown. ---------------- */
static mrb_value
mrb_hypha_platform(mrb_state* /*mrb*/, mrb_value /*self*/)
{
#if defined(WEBVIEW_COCOA)
    return mrb_symbol_value(MRB_SYM(macos));
#elif defined(WEBVIEW_EDGE)
    return mrb_symbol_value(MRB_SYM(windows));
#elif defined(WEBVIEW_GTK)
    return mrb_symbol_value(MRB_SYM(linux));
#else
    return mrb_symbol_value(MRB_SYM(unknown));
#endif
}

/* Hypha.running? — true iff a webview instance exists. ------------------- */
static mrb_value
mrb_hypha_running_p(mrb_state* /*mrb*/, mrb_value /*self*/)
{
    return mrb_bool_value(g_wv.load(std::memory_order_acquire) != nullptr);
}

/* ========================================================================= */
/* gem_init                                                                  */
/*                                                                           */
/* Registers the methods above. tools/hypha/hypha.cpp's hypha_install_      */
/* runtime adds Hypha.run, Hypha.ready, Hypha.bind, Hypha.unbind,           */
/* Hypha.add_native_event, Hypha.remove_native_event, Hypha.bindings,      */
/* Hypha.window_handle, Hypha.handle, plus the error class hierarchy and   */
/* the _FDUD/_WndCtx helper classes.                                       */
/* ========================================================================= */

MRB_BEGIN_DECL
void
mrb_hypha_mrb_gem_init(mrb_state* mrb)
{
    struct RClass* hypha = mrb_define_module_id(mrb, MRB_SYM(Hypha));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM_E(title),
        mrb_hypha_set_title, MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(set_title),
        mrb_hypha_set_title, MRB_ARGS_REQ(1));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM_E(html),
        mrb_hypha_set_html, MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(set_html),
        mrb_hypha_set_html, MRB_ARGS_REQ(1));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM_E(url),
        mrb_hypha_navigate, MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, hypha, MRB_SYM(navigate),
        mrb_hypha_navigate, MRB_ARGS_REQ(1));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(eval),
        mrb_hypha_eval, MRB_ARGS_REQ(1));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(init),
        mrb_hypha_init, MRB_ARGS_REQ(1));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(set_size),
        mrb_hypha_set_size, MRB_ARGS_ARG(2, 1));
    mrb_define_class_method_id(mrb, hypha, MRB_SYM_E(size),
        mrb_hypha_size_setter, MRB_ARGS_REQ(1));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(resolve),
        mrb_hypha_resolve, MRB_ARGS_REQ(3));

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(terminate),
        mrb_hypha_terminate, MRB_ARGS_NONE());

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(dispatch),
        mrb_hypha_dispatch,
        MRB_ARGS_ANY() | MRB_ARGS_BLOCK());

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(version),
        mrb_hypha_version, MRB_ARGS_NONE());

    mrb_define_class_method_id(mrb, hypha, MRB_SYM(platform),
        mrb_hypha_platform, MRB_ARGS_NONE());

    mrb_define_class_method_id(mrb, hypha, MRB_SYM_Q(running),
        mrb_hypha_running_p, MRB_ARGS_NONE());
}

void
mrb_hypha_mrb_gem_final(mrb_state* /*mrb*/)
{
}
MRB_END_DECL