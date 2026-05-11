/*
 * webview_internal.h — shared state between mruby-webview's src/ and the
 *                      tools/hypha/ bin TU.
 *
 * The architecture pins on two globals:
 *
 *   g_main_mrb : the mrb_state owned by main(). EXACTLY ONE mrb_state
 *                exists in a Hypha process. Any thread may serialize
 *                data or procs into bytes, but mrb_state is touched
 *                only on the main thread, inside webview::dispatch
 *                lambdas (or directly from main when the caller is
 *                already there).
 *
 *   g_wv       : the webview::webview instance. Created by Hypha.run on
 *                main, lives until run() returns. Workers reading via
 *                acquire-load see either the valid pointer or null;
 *                both are safe.
 *
 * NOT in include/. Internal to mruby-webview; both src/ and tools/hypha/
 * include this header. Declarations live here, definitions in
 * tools/hypha/hypha.cpp.
 */
#ifndef MRUBY_WEBVIEW_INTERNAL_H
#define MRUBY_WEBVIEW_INTERNAL_H

#include <mruby.h>
#include <webview/webview.h>

#include <atomic>

extern std::atomic<mrb_state*>        g_main_mrb;
extern std::atomic<webview::webview*> g_wv;

/* hypha_in_main_state — true iff the caller's mrb_state is the one main()
 * owns. By construction, only the main thread ever uses g_main_mrb (other
 * threads, if any, have their own mrb_states), so this check answers
 * "are we on main?" more honestly than a thread-id comparison would.
 *
 * Returns false before main() has stored g_main_mrb, which is the right
 * answer ("not on main if main hasn't started"). */
static inline bool
hypha_in_main_state(mrb_state* caller_mrb)
{
    return caller_mrb == g_main_mrb.load(std::memory_order_acquire);
}

/* ========================================================================= */
/* Error helpers — translate webview_error_t -> Hypha::<XxxError>.           */
/*                                                                           */
/* hypha_error_class    : resolve subclass; falls back to Hypha::Error.      */
/* hypha_check          : raise unless code == WEBVIEW_ERROR_OK.             */
/* hypha_check_result   : for webview's `result<T>`; raises on !ok().        */
/*                                                                           */
/* In dispatched lambdas, pass g_main_mrb.load() as the mrb argument: the    */
/* worker's mrb is gone by the time the lambda lands on main.                */
/* ========================================================================= */
#include <mruby/class.h>
#include <mruby/error.h>
#include <mruby/presym.h>
#include <string>

struct RClass* hypha_error_class(mrb_state* mrb, webview_error_t err);
void           hypha_check(mrb_state* mrb, webview_error_t code, const std::string& msg);

template <typename R>
void
hypha_check_result(mrb_state* mrb, const R& r)
{
    if (r.ok()) return;
    const auto& info = r.error();
    hypha_check(mrb, info.code(), info.message());
}

/* Run `fn(m)` on the main thread inside an mrb_protect_error frame, where  */
/* `m` is g_main_mrb. Use from inside webview::dispatch lambdas so any      */
/* raise from hypha_check_result longjmps into the local frame instead of  */
/* unwinding through webview::run(). On error, prints + clears mrb->exc.    */
template <typename F>
inline void
hypha_protect_on_main(F fn)
{
    mrb_state* m = g_main_mrb.load(std::memory_order_acquire);
    if (!m) return;

    mrb_bool err = FALSE;
    mrb_value exc = mrb_protect_error(m,
        [](mrb_state* mm, void* p) -> mrb_value {
            (*static_cast<F*>(p))(mm);
            return mrb_nil_value();
        },
        &fn, &err);
    if (err) {
        /* mrb_protect_error clears mrb->exc and hands the exception back
         * via the return value; restore it so mrb_print_error can print. */
        m->exc = mrb_obj_ptr(exc);
        mrb_print_error(m);
        m->exc = nullptr;
    }
}

#endif /* MRUBY_WEBVIEW_INTERNAL_H */