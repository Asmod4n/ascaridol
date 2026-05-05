/*
 * mruby-webview: idiomatic Ruby bindings for webview/webview.
 *
 * Built on top of webview's C++ engine class (webview::webview) directly,
 * so the API uses std::string everywhere and the bind / dispatch hooks
 * are std::function lambdas captured at registration time. mruby's GC
 * owns the C++ instance via mruby-c-ext-helpers' Data_Make_Struct
 * wrapper (mrb_cpp_new / mrb_cpp_get / mrb_cpp_delete).
 *
 * Copyright (c) 2026 Hendrik Beskow. MIT License.
 */

#include <mruby.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/variable.h>
#include <mruby/class.h>
#include <mruby/proc.h>
#include <mruby/error.h>
#include <mruby/value.h>
#include <mruby/numeric.h>

#include <mruby/num_helpers.hpp>
#include <mruby/cpp_helpers.hpp>
#include <mruby/fast_json.h>

/* No WEBVIEW_HEADER / WEBVIEW_STATIC: we want both the C++ engine class
 * and the C bridge implementations compiled into this single TU. */
#include <webview/webview.h>

#include <functional>
#include <string>

MRB_CPP_DEFINE_TYPE(webview::webview, Webview)

/* ------------------------------------------------------------------------- */
/* Error helpers                                                             */
/* ------------------------------------------------------------------------- */

static struct RClass *
webview_error_class(mrb_state *mrb, webview_error_t err) {
  struct RClass *base = mrb_class_get_id(mrb, MRB_SYM(Webview));
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
webview_check(mrb_state *mrb, webview_error_t code, const std::string &msg) {
  if (code == WEBVIEW_ERROR_OK) return;
  const char *fallback;
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
  mrb_raise(mrb, webview_error_class(mrb, code),
            msg.empty() ? fallback : msg.c_str());
}

template <typename R>
static void
webview_check_result(mrb_state *mrb, const R &r) {
  if (r.ok()) return;
  const auto &info = r.error();
  webview_check(mrb, info.code(), info.message());
}

/* Get the wrapped C++ webview instance, or raise DestroyedError. */
static webview::webview *
get_webview(mrb_state *mrb, mrb_value self) {
  webview::webview *wv = mrb_cpp_get<webview::webview>(mrb, self);
  if (!wv) {
    mrb_raise(mrb,
      mrb_class_get_under_id(mrb, mrb_class_get_id(mrb, MRB_SYM(Webview)), MRB_SYM(DestroyedError)),
      "Webview instance has been destroyed");
  }
  return wv;
}

/* ------------------------------------------------------------------------- */
/* mrb_value <-> std::string helpers                                         */
/* ------------------------------------------------------------------------- */

static std::string
to_std_string(mrb_value v) {
  return std::string{RSTRING_PTR(v), static_cast<size_t>(RSTRING_LEN(v))};
}

static mrb_value
str_from(mrb_state *mrb, const std::string &s) {
  return mrb_str_new(mrb, s.data(), s.size());
}

/* ------------------------------------------------------------------------- */
/* Internal storage on the Webview instance                                  */
/*                                                                           */
/* Two hidden iv tables (the symbol names omit a leading "@", so they don't  */
/* show up in instance_variables but still live in the same iv table) keep   */
/* the user's Ruby objects GC-rooted for as long as the C++ side can reach   */
/* them. `bindings` maps name_sym -> Proc (binding callbacks); `fds_procs`   */
/* maps fd_obj -> Webview::_FDUD instance (native event watchers). The       */
/* lambdas / fd-userdata we hand to webview only carry the lookup key, never */
/* the Proc itself.                                                          */
/* ------------------------------------------------------------------------- */

static mrb_value
iv_hash(mrb_state *mrb, mrb_value self, mrb_sym sym) {
  mrb_value h = mrb_iv_get(mrb, self, sym);
  if (!mrb_hash_p(h)) {
    h = mrb_hash_new(mrb);
    mrb_iv_set(mrb, self, sym, h);
  }
  return h;
}

#define BINDINGS_HASH(mrb, self)  iv_hash(mrb, self, MRB_SYM(bindings))
#define FDS_HASH(mrb, self)       iv_hash(mrb, self, MRB_SYM(fds_procs))

/* ------------------------------------------------------------------------- */
/* Bind callback machinery (lambda body)                                     */
/* ------------------------------------------------------------------------- */

struct bind_step {
  mrb_value src;
  mrb_value proc;
  mrb_value parsed;
  mrb_value result;
};

static mrb_value
bind_parse_body(mrb_state *mrb, void *p) {
  auto *s = static_cast<bind_step *>(p);
  struct RClass *json = mrb_module_get_id(mrb, MRB_SYM(JSON));
  return mrb_funcall_id(mrb, mrb_obj_value(json), MRB_SYM(parse), 1, s->src);
}

static mrb_value
bind_invoke_body(mrb_state *mrb, void *p) {
  auto *s = static_cast<bind_step *>(p);
  mrb_int argc = RARRAY_LEN(s->parsed);
  mrb_value *argv = RARRAY_PTR(s->parsed);
  return mrb_yield_argv(mrb, s->proc, argc, argv);
}

static mrb_value
bind_dump_body(mrb_state *mrb, void *p) {
  auto *s = static_cast<bind_step *>(p);
  return mrb_json_dump(mrb, s->result);
}

static std::string
make_error_json_str(mrb_state *mrb, mrb_value name, mrb_value message) {
  mrb_value h = mrb_hash_new_capa(mrb, 2);
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(name)),    name);
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(message)), message);
  return to_std_string(mrb_json_dump(mrb, h));
}

/* Runs synchronously on the UI thread (== the mruby thread): looks up the
 * Ruby block in the hidden `bindings` iv table via name_sym, parses the
 * JSON args, invokes the block, and resolves the JS-side promise via
 * wv->resolve. Exceptions from JSON parse / proc call / JSON dump get
 * mapped onto resolve(id, 1, "{name:, message:}"). */
static void
invoke_bound_proc(mrb_state *mrb, mrb_value self, mrb_sym name_sym,
                  webview::webview *wv,
                  const std::string &id, const std::string &req) {
  int ai = mrb_gc_arena_save(mrb);

  mrb_value proc = mrb_hash_fetch(mrb, BINDINGS_HASH(mrb, self),
                                mrb_symbol_value(name_sym), mrb_undef_value());
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
    wv->resolve(id, 1,
      make_error_json_str(mrb, mrb_str_new_lit(mrb, "ParseError"), msg));
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
    mrb_value msg  = mrb_funcall_id(mrb, result, MRB_SYM(message), 0);
    mrb_value cls  = mrb_funcall_id(mrb, result, MRB_SYM(class), 0);
    mrb_value name = mrb_funcall_id(mrb, cls, MRB_SYM(name), 0);
    if (!mrb_string_p(name)) name = mrb_str_new_lit(mrb, "Error");
    wv->resolve(id, 1, make_error_json_str(mrb, name, msg));
    mrb_gc_arena_restore(mrb, ai);
    return;
  }
  step.result = result;

  err = FALSE;
  mrb_value json_result = mrb_protect_error(mrb, bind_dump_body, &step, &err);
  std::string out = err ? std::string{"null"} : to_std_string(json_result);
  wv->resolve(id, 0, out);
  mrb_gc_arena_restore(mrb, ai);
}

/* ------------------------------------------------------------------------- */
/* initialize / destroy                                                      */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_initialize(mrb_state *mrb, mrb_value self) {
  mrb_bool debug = FALSE;
  mrb_value window_handle = mrb_nil_value();
  mrb_get_args(mrb, "|bo!", &debug, &window_handle);

  void *window = nullptr;
  if (mrb_cptr_p(window_handle)) {
    window = mrb_cptr(window_handle);
  } else if (!mrb_nil_p(window_handle)) {
    mrb_raise(mrb, E_TYPE_ERROR, "window must be nil or an cptr (native handle)");
  }

  if (DATA_PTR(self)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "already initialized");
  }

  try {
    mrb_cpp_new<webview::webview>(mrb, self, static_cast<bool>(debug), window);
  } catch (const webview::exception &e) {
    webview_check(mrb, e.error().code(), e.error().message());
  }
  return self;
}

static mrb_value
mrb_webview_m_destroy(mrb_state *mrb, mrb_value self) {
  webview::webview *wv = mrb_cpp_get<webview::webview>(mrb, self);
  if (wv) {
    /* The C++ destructor unbinds every binding and tears down the
     * platform window (GTK / Cocoa / WebView2). The std::function lambdas
     * we passed to bind are destroyed with it; their captures (mrb, self,
     * name_sym, wv) are released. Wiping the hidden `bindings` iv table
     * drops the last references to the user's blocks so GC reclaims
     * them; wiping `fds_procs` releases the Webview::_FDUD wrappers,
     * whose destructors detach any still-attached GLib fd watchers. */
    mrb_cpp_delete<webview::webview>(mrb, wv);
    DATA_PTR(self) = nullptr;
    DATA_TYPE(self) = nullptr;
    mrb_iv_remove(mrb, self, MRB_SYM(bindings));
    mrb_iv_remove(mrb, self, MRB_SYM(fds_procs));
  }
  return mrb_nil_value();
}

static mrb_value
mrb_webview_m_destroyed_p(mrb_state *mrb, mrb_value self) {
  (void)mrb;
  return mrb_bool_value(!DATA_PTR(self));
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_run(mrb_state *mrb, mrb_value self) {
  webview_check_result(mrb, get_webview(mrb, self)->run());
  return self;
}

static mrb_value
mrb_webview_m_terminate(mrb_state *mrb, mrb_value self) {
  webview_check_result(mrb, get_webview(mrb, self)->terminate());
  return self;
}

/* ------------------------------------------------------------------------- */
/* Window manipulation (std::string everywhere)                              */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_set_title(mrb_state *mrb, mrb_value self) {
  const char *title;
  mrb_int len;
  mrb_get_args(mrb, "s", &title, &len);
  webview_check_result(mrb, get_webview(mrb, self)->set_title(
    std::string{title, static_cast<size_t>(len)}));
  return self;
}

static webview_hint_t
hint_from_mrb(mrb_state *mrb, mrb_value v) {
  if (mrb_nil_p(v))     return WEBVIEW_HINT_NONE;
  if (mrb_symbol_p(v)) {
    mrb_sym s = mrb_symbol(v);
    if (s == MRB_SYM(none))  return WEBVIEW_HINT_NONE;
    if (s == MRB_SYM(min))   return WEBVIEW_HINT_MIN;
    if (s == MRB_SYM(max))   return WEBVIEW_HINT_MAX;
    if (s == MRB_SYM(fixed)) return WEBVIEW_HINT_FIXED;
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown size hint: %v", v);
  }
  mrb_raise(mrb, E_ARGUMENT_ERROR, "size hint must be a Symbol");
}

static mrb_value
mrb_webview_m_set_size(mrb_state *mrb, mrb_value self) {
  mrb_int w, h;
  mrb_value hint_v = mrb_nil_value();
  mrb_get_args(mrb, "ii|o!", &w, &h, &hint_v);
  webview::webview *wv = get_webview(mrb, self);
  webview_hint_t hints = hint_from_mrb(mrb, hint_v);


  webview_check_result(mrb, wv->set_size(static_cast<int>(w), static_cast<int>(h), hints));
  return self;
}

static mrb_value
mrb_webview_m_navigate(mrb_state *mrb, mrb_value self) {
  const char *url;
  mrb_int len;
  mrb_get_args(mrb, "s", &url, &len);
  webview_check_result(mrb, get_webview(mrb, self)->navigate(
    std::string{url, static_cast<size_t>(len)}));
  return self;
}

static mrb_value
mrb_webview_m_set_html(mrb_state *mrb, mrb_value self) {
  const char *html;
  mrb_int len;
  mrb_get_args(mrb, "s", &html, &len);
  webview_check_result(mrb, get_webview(mrb, self)->set_html(
    std::string{html, static_cast<size_t>(len)}));
  return self;
}

static mrb_value
mrb_webview_m_init(mrb_state *mrb, mrb_value self) {
  const char *js;
  mrb_int len;
  mrb_get_args(mrb, "s", &js, &len);
  webview_check_result(mrb, get_webview(mrb, self)->init(
    std::string{js, static_cast<size_t>(len)}));
  return self;
}

static mrb_value
mrb_webview_m_eval(mrb_state *mrb, mrb_value self) {
  const char *js;
  mrb_int len;
  mrb_get_args(mrb, "s", &js, &len);
  webview_check_result(mrb, get_webview(mrb, self)->eval(
    std::string{js, static_cast<size_t>(len)}));
  return self;
}

/* ------------------------------------------------------------------------- */
/* Native handles                                                            */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_window_handle(mrb_state *mrb, mrb_value self) {
  auto r = get_webview(mrb, self)->window();
  if (!r.ok()) return mrb_nil_value();
  void *p = r.value();
  return p ? mrb_cptr_value(mrb, p)
           : mrb_nil_value();
}

static webview_native_handle_kind_t
handle_kind_from_mrb(mrb_state *mrb, mrb_value v) {
  if (mrb_symbol_p(v)) {
    mrb_sym s = mrb_symbol(v);
    if (s == MRB_SYM(window))             return WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW;
    if (s == MRB_SYM(widget))             return WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET;
    if (s == MRB_SYM(browser_controller)) return WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER;
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown handle kind: %v", v);
  }
  mrb_raise(mrb, E_ARGUMENT_ERROR, "handle kind must be a Symbol");
}

static mrb_value
mrb_webview_m_native_handle(mrb_state *mrb, mrb_value self) {
  webview::webview *wv = get_webview(mrb, self);
  mrb_value kind_v = mrb_symbol_value(MRB_SYM(window));
  mrb_get_args(mrb, "|o", &kind_v);

  void *p = nullptr;
  switch (handle_kind_from_mrb(mrb, kind_v)) {
    case WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW:
      { auto r = wv->window();              if (r.ok()) p = r.value(); break; }
    case WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET:
      { auto r = wv->widget();              if (r.ok()) p = r.value(); break; }
    case WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER:
      { auto r = wv->browser_controller(); if (r.ok()) p = r.value(); break; }
  }
  return p ? mrb_cptr_value(mrb, p)
           : mrb_nil_value();
}

/* ------------------------------------------------------------------------- */
/* bind / unbind / return                                                    */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_bind(mrb_state *mrb, mrb_value self) {
  webview::webview *wv = get_webview(mrb, self);
  mrb_sym name_sym;
  mrb_value blk = mrb_nil_value();
  mrb_get_args(mrb, "n&", &name_sym, &blk);
  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bind requires a block");
  }

  mrb_value bh = BINDINGS_HASH(mrb, self);

  /* Re-bind on an already-registered name: just swap the proc — webview
   * itself would return WEBVIEW_ERROR_DUPLICATE if we called bind() again. */
  if (!mrb_nil_p(mrb_hash_get(mrb, bh, mrb_symbol_value(name_sym)))) {
    mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), blk);
    return self;
  }

  /* Fresh registration: hand webview a std::function lambda capturing
   * (mrb, self, name_sym, wv). The hidden `bindings` iv table keeps the
   * user's block GC-rooted while the binding is live; the lambda just
   * does a hash lookup on every JS call. The lambda outlives nothing it
   * captures — webview destroys every binding in its destructor, which
   * runs before the wrapping Webview Data instance is freed. */
  mrb_int name_len;
  const char *name = mrb_sym_name_len(mrb, name_sym, &name_len);
  std::string name_str{name, static_cast<size_t>(name_len)};
  auto err = wv->bind(name_str,
    [mrb, self, name_sym, wv](std::string id, std::string req, void *) {
      invoke_bound_proc(mrb, self, name_sym, wv, id, req);
    },
    nullptr);
  webview_check_result(mrb, err);

  mrb_hash_set(mrb, bh, mrb_symbol_value(name_sym), blk);
  return self;
}

static mrb_value
mrb_webview_m_unbind(mrb_state *mrb, mrb_value self) {
  webview::webview *wv = get_webview(mrb, self);
  mrb_sym name_sym;
  mrb_get_args(mrb, "n", &name_sym);

  mrb_int name_len;
  const char *name = mrb_sym_name_len(mrb, name_sym, &name_len);
  webview_check_result(mrb, wv->unbind(
    std::string{name, static_cast<size_t>(name_len)}));

  mrb_hash_delete_key(mrb, BINDINGS_HASH(mrb, self), mrb_symbol_value(name_sym));
  return self;
}

static mrb_value
mrb_webview_m_return(mrb_state *mrb, mrb_value self) {
  webview::webview *wv = get_webview(mrb, self);
  const char *id, *result;
  mrb_int id_len, result_len, status;
  mrb_get_args(mrb, "sis", &id, &id_len, &status, &result, &result_len);
  webview_check_result(mrb, wv->resolve(
    std::string{id, static_cast<size_t>(id_len)},
    static_cast<int>(status),
    std::string{result, static_cast<size_t>(result_len)}));
  return self;
}

static mrb_value
mrb_webview_m_bindings(mrb_state *mrb, mrb_value self) {
  mrb_value bh = mrb_iv_get(mrb, self, MRB_SYM(bindings));
  if (!mrb_hash_p(bh)) return mrb_ary_new(mrb);

  mrb_value keys = mrb_hash_keys(mrb, bh);
  mrb_int len = RARRAY_LEN(keys);
  mrb_value result = mrb_ary_new_capa(mrb, len);

  for (mrb_int i = 0; i < len; i++) {
    mrb_ary_push(mrb, result, mrb_ary_ref(mrb, keys, i));
  }
  return result;
}

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_s_version(mrb_state *mrb, mrb_value self) {
  (void)self;
  const webview_version_info_t *info = webview_version();
  mrb_value h = mrb_hash_new(mrb);
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(major)),
               mrb_convert_number(mrb, info->version.major));
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(minor)),
               mrb_convert_number(mrb, info->version.minor));
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(patch)),
               mrb_convert_number(mrb, info->version.patch));
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(version)),
               mrb_str_new_cstr(mrb, info->version_number));
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(pre_release)),
               mrb_str_new_cstr(mrb, info->pre_release));
  mrb_hash_set(mrb, h, mrb_symbol_value(MRB_SYM(build_metadata)),
               mrb_str_new_cstr(mrb, info->build_metadata));
  return h;
}

#ifdef WEBVIEW_GTK
#include <glib-unix.h>

struct mrb_webview_fd_ud {
  mrb_state *mrb;
  mrb_value wv;
  mrb_value fd;
  mrb_value blk;
  guint id = 0;

  ~mrb_webview_fd_ud() {
    if (id) g_source_remove(id);
  }
};

MRB_CPP_DEFINE_TYPE(mrb_webview_fd_ud, Webview_Userdata);

gboolean on_fd_ready(gint fd, GIOCondition condition, gpointer user_data) {
  struct mrb_webview_fd_ud *fd_ud = static_cast<mrb_webview_fd_ud*>(user_data);
  mrb_state *mrb = fd_ud->mrb;

  mrb_int idx = mrb_gc_arena_save(mrb);
  const mrb_value argv[] = {
    fd_ud->fd, mrb_convert_number(mrb, condition)
  };
  mrb_bool cont = mrb_test(mrb_yield_argv(mrb, fd_ud->blk, 2, argv));
  if (!cont) {
    mrb_value fds_hash = FDS_HASH(mrb, fd_ud->wv);
    fd_ud->id = 0;
    mrb_hash_delete_key(mrb, fds_hash, fd_ud->fd);
  }
  mrb_gc_arena_restore(mrb, idx);
  return (gboolean) cont;
}

static mrb_value
mrb_webview_add_native_event(mrb_state *mrb, mrb_value self)
{
  mrb_value fd_obj, blk = mrb_nil_value();
  mrb_get_args(mrb, "o&", &fd_obj, &blk);
  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  if (!mrb_proc_p(blk)) {
    mrb_raise(mrb, E_TYPE_ERROR, "not a block");
  }
  mrb_value fds_hash = FDS_HASH(mrb, self);
  int fd = (int) mrb_integer(mrb_type_convert(mrb, fd_obj, MRB_TT_INTEGER, MRB_SYM(fileno)));

  const mrb_value argv[] = {
    self, fd_obj, blk
  };
  mrb_value fd_ud_obj = mrb_obj_new(mrb, mrb_class_get_under_id(mrb, mrb_class(mrb, self), MRB_SYM(_FDUD)), 3, argv);
  mrb_hash_set(mrb, fds_hash, fd_obj, fd_ud_obj);

  mrb_webview_fd_ud *fd_ud = mrb_cpp_get<mrb_webview_fd_ud>(mrb, fd_ud_obj);
  fd_ud->id = g_unix_fd_add(fd, G_IO_IN, on_fd_ready, DATA_PTR(fd_ud_obj));
  return self;
}

static mrb_value
mrb_webview_remove_native_event(mrb_state *mrb, mrb_value self)
{
  mrb_value fd_obj;
  mrb_get_args(mrb, "o", &fd_obj);
  mrb_value fds_hash = FDS_HASH(mrb, self);
  mrb_value fd_ud_obj = mrb_hash_fetch(mrb, fds_hash, fd_obj, mrb_undef_value());
  if (!mrb_undef_p(fd_ud_obj)) {
    mrb_webview_fd_ud *fd_ud = mrb_cpp_get<mrb_webview_fd_ud>(mrb, fd_ud_obj);
    g_source_remove(fd_ud->id);
    fd_ud->id = 0;
    mrb_hash_delete_key(mrb, fds_hash, fd_obj);
  }
  return self;
}
#endif

#ifdef WEBVIEW_COCOA
#include <CoreFoundation/CoreFoundation.h>

struct mrb_webview_fd_ud {
  mrb_state *mrb;
  mrb_value wv;
  mrb_value fd;
  mrb_value blk;
  CFFileDescriptorRef cf_fd = nullptr;
  CFRunLoopSourceRef  src   = nullptr;

  ~mrb_webview_fd_ud() {
    if (src) {
      CFRunLoopRemoveSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
      CFRelease(src);
      src = nullptr;
    }
    if (cf_fd) {
      // closeOnInvalidate was false at create time → fd is NOT closed, same as GTK
      CFFileDescriptorInvalidate(cf_fd);
      CFRelease(cf_fd);
      cf_fd = nullptr;
    }
  }
};

MRB_CPP_DEFINE_TYPE(mrb_webview_fd_ud, Webview_Userdata);

static mrb_value
mrb_webview_userdata_init(mrb_state *mrb, mrb_value self)
{
  mrb_value wv, fd, blk;
  mrb_get_args(mrb, "ooo", &wv, &fd, &blk);

  mrb_iv_set(mrb, self, MRB_SYM(wv),  wv);
  mrb_iv_set(mrb, self, MRB_SYM(fd),  fd);
  mrb_iv_set(mrb, self, MRB_SYM(blk), blk);

  mrb_cpp_new<mrb_webview_fd_ud>(mrb, self);
  mrb_webview_fd_ud *ud = mrb_cpp_get<mrb_webview_fd_ud>(mrb, self);
  ud->mrb = mrb;
  ud->wv  = wv;
  ud->fd  = fd;
  ud->blk = blk;
  return self;
}

static void
on_cf_fd_ready(CFFileDescriptorRef cf_fd, CFOptionFlags /*callbackTypes*/, void *info)
{
  mrb_webview_fd_ud *ud = static_cast<mrb_webview_fd_ud*>(info);
  mrb_state *mrb = ud->mrb;

  mrb_int ai = mrb_gc_arena_save(mrb);
  // Mirror the GTK condition: only ever read, so pass G_IO_IN == 1
  // so user blocks are portable across backends.
  const mrb_value argv[] = { ud->fd, mrb_int_value(mrb, 1) };
  mrb_bool cont = mrb_test(mrb_yield_argv(mrb, ud->blk, 2, argv));

  if (cont) {
    // CFFileDescriptor callbacks are one-shot — re-arm for the next read.
    CFFileDescriptorEnableCallBacks(cf_fd, kCFFileDescriptorReadCallBack);
  } else {
    mrb_value fds_hash = FDS_HASH(mrb, ud->wv);
    mrb_hash_delete_key(mrb, fds_hash, ud->fd);
    // Drop completes via the _FDUD destructor (removes source, releases CF refs).
  }
  mrb_gc_arena_restore(mrb, ai);
}

static mrb_value
mrb_webview_add_native_event(mrb_state *mrb, mrb_value self)
{
  mrb_value fd_obj, blk = mrb_nil_value();
  mrb_get_args(mrb, "o&", &fd_obj, &blk);
  if (mrb_nil_p(blk))   mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  if (!mrb_proc_p(blk)) mrb_raise(mrb, E_TYPE_ERROR, "not a block");

  mrb_value fds_hash = FDS_HASH(mrb, self);
  int fd = (int) mrb_integer(
    mrb_type_convert(mrb, fd_obj, MRB_TT_INTEGER, MRB_SYM(fileno)));

  const mrb_value argv[] = { self, fd_obj, blk };
  mrb_value ud_obj = mrb_obj_new(mrb,
    mrb_class_get_under_id(mrb, mrb_class(mrb, self), MRB_SYM(_FDUD)), 3, argv);
  mrb_hash_set(mrb, fds_hash, fd_obj, ud_obj);

  mrb_webview_fd_ud *ud = mrb_cpp_get<mrb_webview_fd_ud>(mrb, ud_obj);

  CFFileDescriptorContext ctx = { 0, ud, nullptr, nullptr, nullptr };
  ud->cf_fd = CFFileDescriptorCreate(kCFAllocatorDefault, fd,
                                     /*closeOnInvalidate=*/false,
                                     on_cf_fd_ready, &ctx);
  if (!ud->cf_fd) {
    mrb_hash_delete_key(mrb, fds_hash, fd_obj);
    mrb_raise(mrb, E_RUNTIME_ERROR, "CFFileDescriptorCreate failed");
  }
  CFFileDescriptorEnableCallBacks(ud->cf_fd, kCFFileDescriptorReadCallBack);

  ud->src = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, ud->cf_fd, 0);
  if (!ud->src) {
    mrb_hash_delete_key(mrb, fds_hash, fd_obj);
    mrb_raise(mrb, E_RUNTIME_ERROR, "CFFileDescriptorCreateRunLoopSource failed");
  }
  CFRunLoopAddSource(CFRunLoopGetMain(), ud->src, kCFRunLoopCommonModes);
  return self;
}

static mrb_value
mrb_webview_remove_native_event(mrb_state *mrb, mrb_value self)
{
  mrb_value fd_obj;
  mrb_get_args(mrb, "o", &fd_obj);
  mrb_value fds_hash = FDS_HASH(mrb, self);
  mrb_value ud_obj = mrb_hash_fetch(mrb, fds_hash, fd_obj, mrb_undef_value());
  if (!mrb_undef_p(ud_obj)) {
    mrb_hash_delete_key(mrb, fds_hash, fd_obj);  // destructor does the teardown
  }
  return self;
}
#endif

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */
MRB_BEGIN_DECL
void
mrb_mruby_webview_gem_init(mrb_state *mrb) {
  struct RClass *cls = mrb_define_class_id(mrb, MRB_SYM(Webview), mrb->object_class);
  MRB_SET_INSTANCE_TT(cls, MRB_TT_CDATA);
#if defined(WEBVIEW_GTK) || defined(WEBVIEW_COCOA)
  struct RClass *ud_cls = mrb_define_class_under_id(mrb, cls, MRB_SYM(_FDUD), mrb->object_class);
  mrb_define_method_id(mrb, ud_cls, MRB_SYM(initialize), mrb_webview_userdata_init, MRB_ARGS_REQ(2));
  MRB_SET_INSTANCE_TT(ud_cls, MRB_TT_CDATA);
#endif

  struct RClass *err = mrb_define_class_under_id(mrb, cls, MRB_SYM(Error), E_RUNTIME_ERROR);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(MissingDependencyError), err);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(CanceledError),          err);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(InvalidStateError),      err);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(InvalidArgumentError),   err);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(DuplicateError),         err);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(NotFoundError),          err);
  mrb_define_class_under_id(mrb, cls, MRB_SYM(DestroyedError),         err);

  mrb_define_method_id(mrb, cls, MRB_SYM(initialize),    mrb_webview_m_initialize,    MRB_ARGS_OPT(2));
  mrb_define_method_id(mrb, cls, MRB_SYM(destroy),       mrb_webview_m_destroy,       MRB_ARGS_NONE());
  mrb_define_method_id(mrb, cls, MRB_SYM_Q(destroyed),   mrb_webview_m_destroyed_p,   MRB_ARGS_NONE());

  mrb_define_method_id(mrb, cls, MRB_SYM(run),           mrb_webview_m_run,           MRB_ARGS_NONE());
  mrb_define_method_id(mrb, cls, MRB_SYM(terminate),     mrb_webview_m_terminate,     MRB_ARGS_NONE());

  mrb_define_method_id(mrb, cls, MRB_SYM_E(title),       mrb_webview_m_set_title,     MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(set_title),     mrb_webview_m_set_title,     MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(set_size),      mrb_webview_m_set_size,      MRB_ARGS_ARG(2,1));
  mrb_define_method_id(mrb, cls, MRB_SYM(navigate),      mrb_webview_m_navigate,      MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM_E(url),         mrb_webview_m_navigate,      MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(set_html),      mrb_webview_m_set_html,      MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM_E(html),        mrb_webview_m_set_html,      MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(init),           mrb_webview_m_init,         MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(eval),           mrb_webview_m_eval,         MRB_ARGS_REQ(1));

  mrb_define_method_id(mrb, cls, MRB_SYM(window_handle), mrb_webview_m_window_handle, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, cls, MRB_SYM(handle), mrb_webview_m_native_handle, MRB_ARGS_OPT(1));

  mrb_define_method_id(mrb, cls, MRB_SYM(_bind_native),  mrb_webview_m_bind,          MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
  mrb_define_method_id(mrb, cls, MRB_SYM(unbind),        mrb_webview_m_unbind,        MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(return_result), mrb_webview_m_return,        MRB_ARGS_REQ(3));

  mrb_define_method_id(mrb, cls, MRB_SYM(bindings),      mrb_webview_m_bindings,      MRB_ARGS_NONE());
#if defined(WEBVIEW_GTK) || defined(WEBVIEW_COCOA)
  mrb_define_method_id(mrb, cls, MRB_SYM(add_native_event), mrb_webview_add_native_event, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_method_id(mrb, cls, MRB_SYM(remove_native_event), mrb_webview_remove_native_event, MRB_ARGS_REQ(1));
#endif
  mrb_define_class_method_id(mrb, cls, MRB_SYM(version), mrb_webview_s_version,       MRB_ARGS_NONE());
}

void
mrb_mruby_webview_gem_final(mrb_state *mrb) {
  (void)mrb;
}
MRB_END_DECL
