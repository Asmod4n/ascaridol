// Single-translation-unit build of webview/webview's C++ implementation.
//
// We don't add vendor/webview/core/src/webview.cc to spec.objs directly,
// because the exact moment at which mruby's gem setup pipeline reads
// spec.objs (and how it dispatches custom file rules) varies between
// mruby versions and can drop externally-added object files. By placing
// this thin wrapper inside src/, mruby's auto-glob picks it up like any
// other gem source, compiles it with the configured C++ toolchain, and
// archives the result into libmruby.a alongside the bindings in
// mrb_webview.c.
//
// The relative include resolves against this file's directory, so it
// keeps working regardless of -I order or the build directory layout.
#include "../vendor/webview/core/src/webview.cc"
