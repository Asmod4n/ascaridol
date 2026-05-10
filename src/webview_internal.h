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

#endif /* MRUBY_WEBVIEW_INTERNAL_H */