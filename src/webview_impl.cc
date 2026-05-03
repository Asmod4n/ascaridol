/*
 * Single translation unit for webview's C++ implementation.
 *
 * webview/webview.h uses a WEBVIEW_HEADER guard: when defined, the header
 * exposes only declarations; when undefined (the default), it pulls the full
 * implementation into the current translation unit. We rely on that here
 * exactly once, while every other compilation unit in this gem (notably
 * src/mrb_webview.cc) defines WEBVIEW_HEADER before including the header.
 *
 * mruby's gem build auto-globs src/ so this file is compiled with the build's
 * configured C++ toolchain (and the spec.cxx flags from mrbgem.rake), and the
 * resulting object lands in libmruby.a alongside the bindings.
 */
#include <webview/webview.h>
