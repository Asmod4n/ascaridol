/*
 * mruby-webview: idiomatic Ruby bindings for webview/webview.
 *
 * Copyright (c) 2026 Hendrik. MIT License.
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

/* The C API symbols (webview_create, webview_destroy, ...) live inside
 * libwebview.a, which is built from vendor/webview via its official CMake
 * project (see mrbgem.rake). Defining WEBVIEW_HEADER here gives us
 * declarations only; WEBVIEW_STATIC (set in mrbgem.rake too) makes those
 * declarations have plain `extern` linkage so they resolve at link time
 * against the static library. */
#define WEBVIEW_HEADER
#include <webview/webview.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Data wrapper                                                              */
/* ------------------------------------------------------------------------- */

typedef struct mrb_webview_s {
  webview_t handle;
  mrb_state *mrb;
  mrb_value self;
} mrb_webview_t;

static void mrb_webview_free(mrb_state *mrb, void *p);

static const struct mrb_data_type mrb_webview_data_type = {
  "Webview", mrb_webview_free
};

static mrb_webview_t *
mrb_webview_get(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = (mrb_webview_t *)mrb_data_get_ptr(mrb, self, &mrb_webview_data_type);
  if (!wv || !wv->handle) {
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_class_get(mrb, "Webview"), "DestroyedError"),
              "Webview instance has been destroyed");
  }
  return wv;
}

/* ------------------------------------------------------------------------- */
/* Error helpers                                                             */
/* ------------------------------------------------------------------------- */

static struct RClass *
mrb_webview_error_class(mrb_state *mrb, webview_error_t err) {
  struct RClass *base = mrb_class_get(mrb, "Webview");
  const char *name;
  switch (err) {
    case WEBVIEW_ERROR_MISSING_DEPENDENCY: name = "MissingDependencyError"; break;
    case WEBVIEW_ERROR_CANCELED:           name = "CanceledError";          break;
    case WEBVIEW_ERROR_INVALID_STATE:      name = "InvalidStateError";      break;
    case WEBVIEW_ERROR_INVALID_ARGUMENT:   name = "InvalidArgumentError";   break;
    case WEBVIEW_ERROR_DUPLICATE:          name = "DuplicateError";         break;
    case WEBVIEW_ERROR_NOT_FOUND:          name = "NotFoundError";          break;
    case WEBVIEW_ERROR_UNSPECIFIED:
    default:                               name = "Error";                  break;
  }
  return mrb_class_get_under(mrb, base, name);
}

static void
mrb_webview_check(mrb_state *mrb, webview_error_t err) {
  if (err == WEBVIEW_ERROR_OK) return;
  const char *msg;
  switch (err) {
    case WEBVIEW_ERROR_MISSING_DEPENDENCY: msg = "missing dependency"; break;
    case WEBVIEW_ERROR_CANCELED:           msg = "operation canceled"; break;
    case WEBVIEW_ERROR_INVALID_STATE:      msg = "invalid state"; break;
    case WEBVIEW_ERROR_INVALID_ARGUMENT:   msg = "invalid argument"; break;
    case WEBVIEW_ERROR_DUPLICATE:          msg = "duplicate"; break;
    case WEBVIEW_ERROR_NOT_FOUND:          msg = "not found"; break;
    case WEBVIEW_ERROR_UNSPECIFIED:
    default:                               msg = "unspecified error"; break;
  }
  mrb_raise(mrb, mrb_webview_error_class(mrb, err), msg);
}

/* ------------------------------------------------------------------------- */
/* JSON helpers (delegate to mruby-json)                                     */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_json_parse(mrb_state *mrb, const char *src) {
  struct RClass *json = mrb_module_get(mrb, "JSON");
  return mrb_funcall(mrb, mrb_obj_value(json), "parse", 1, mrb_str_new_cstr(mrb, src));
}

static mrb_value
mrb_webview_json_dump(mrb_state *mrb, mrb_value v) {
  struct RClass *json = mrb_module_get(mrb, "JSON");
  return mrb_funcall(mrb, mrb_obj_value(json), "dump", 1, v);
}

/* ------------------------------------------------------------------------- */
/* Internal storage on the Ruby instance                                     */
/* ------------------------------------------------------------------------- */

static mrb_value
iv_hash(mrb_state *mrb, mrb_value self, const char *name) {
  mrb_sym sym = mrb_intern_cstr(mrb, name);
  mrb_value h = mrb_iv_get(mrb, self, sym);
  if (!mrb_hash_p(h)) {
    h = mrb_hash_new(mrb);
    mrb_iv_set(mrb, self, sym, h);
  }
  return h;
}

#define BINDINGS_HASH(mrb, self) iv_hash(mrb, self, "@_bindings")
#define CTXS_HASH(mrb, self)     iv_hash(mrb, self, "@_binding_ctxs")
#define DISPATCH_HASH(mrb, self) iv_hash(mrb, self, "@_dispatch_procs")

/* ------------------------------------------------------------------------- */
/* Bind callback machinery                                                   */
/* ------------------------------------------------------------------------- */

typedef struct binding_ctx {
  mrb_webview_t *wv;
  mrb_sym name_sym;
} binding_ctx;

static mrb_value
make_error_json(mrb_state *mrb, const char *name, mrb_value message) {
  mrb_value err = mrb_hash_new(mrb);
  mrb_hash_set(mrb, err, mrb_str_new_cstr(mrb, "name"),    mrb_str_new_cstr(mrb, name));
  mrb_hash_set(mrb, err, mrb_str_new_cstr(mrb, "message"), message);
  return mrb_webview_json_dump(mrb, err);
}

/* Per-step state passed through mrb_protect_error. Output fields are
 * filled in by the body functions and consumed by binding_callback. */
struct bind_step {
  const char *req;     /* in:  raw JSON args                        */
  mrb_value proc;      /* in:  Ruby proc to invoke                  */
  mrb_value parsed;    /* in/out: parsed args for the invoke step   */
  mrb_value result;    /* in:  result for the dump step             */
};

static mrb_value
bind_parse_body(mrb_state *mrb, void *p) {
  struct bind_step *s = (struct bind_step *)p;
  return mrb_webview_json_parse(mrb, s->req);
}

static mrb_value
bind_invoke_body(mrb_state *mrb, void *p) {
  struct bind_step *s = (struct bind_step *)p;
  mrb_int argc = RARRAY_LEN(s->parsed);
  mrb_value *argv = RARRAY_PTR(s->parsed);
  return mrb_funcall_with_block(mrb, s->proc, mrb_intern_lit(mrb, "call"),
                                argc, argv, mrb_nil_value());
}

static mrb_value
bind_dump_body(mrb_state *mrb, void *p) {
  struct bind_step *s = (struct bind_step *)p;
  return mrb_webview_json_dump(mrb, s->result);
}

static void
binding_callback(const char *id, const char *req, void *arg) {
  binding_ctx *ctx = (binding_ctx *)arg;
  mrb_webview_t *wv = ctx->wv;
  mrb_state *mrb = wv->mrb;
  mrb_value self = wv->self;
  webview_t handle = wv->handle;

  int ai = mrb_gc_arena_save(mrb);

  mrb_value proc = mrb_hash_get(mrb, BINDINGS_HASH(mrb, self),
                                mrb_symbol_value(ctx->name_sym));
  if (mrb_nil_p(proc)) {
    webview_return(handle, id, 1,
                   "{\"name\":\"Error\",\"message\":\"binding not registered\"}");
    mrb_gc_arena_restore(mrb, ai);
    return;
  }

  struct bind_step step = { req, proc, mrb_nil_value(), mrb_nil_value() };
  mrb_bool err = FALSE;

  /* Step 1: parse JSON arguments. */
  mrb_value parsed = mrb_protect_error(mrb, bind_parse_body, &step, &err);
  if (err) {
    mrb_value msg = mrb_funcall(mrb, parsed, "message", 0);
    mrb_value json = make_error_json(mrb, "ParseError", msg);
    webview_return(handle, id, 1, mrb_string_value_cstr(mrb, &json));
    mrb_gc_arena_restore(mrb, ai);
    return;
  }
  if (!mrb_array_p(parsed)) {
    mrb_value tmp = mrb_ary_new_capa(mrb, 1);
    mrb_ary_push(mrb, tmp, parsed);
    parsed = tmp;
  }
  step.parsed = parsed;

  /* Step 2: invoke the bound proc. */
  err = FALSE;
  mrb_value result = mrb_protect_error(mrb, bind_invoke_body, &step, &err);
  if (err) {
    mrb_value msg  = mrb_funcall(mrb, result, "message", 0);
    mrb_value cls  = mrb_funcall(mrb, result, "class", 0);
    mrb_value name = mrb_funcall(mrb, cls, "name", 0);
    const char *cname = mrb_string_p(name) ? mrb_string_value_cstr(mrb, &name) : "Error";
    mrb_value json = make_error_json(mrb, cname, msg);
    webview_return(handle, id, 1, mrb_string_value_cstr(mrb, &json));
    mrb_gc_arena_restore(mrb, ai);
    return;
  }
  step.result = result;

  /* Step 3: serialize the result. */
  err = FALSE;
  mrb_value json_result = mrb_protect_error(mrb, bind_dump_body, &step, &err);
  if (err) {
    json_result = mrb_str_new_lit(mrb, "null");
  }

  webview_return(handle, id, 0, mrb_string_value_cstr(mrb, &json_result));
  mrb_gc_arena_restore(mrb, ai);
}

/* ------------------------------------------------------------------------- */
/* Dispatch callback machinery                                               */
/* ------------------------------------------------------------------------- */

typedef struct dispatch_ctx {
  mrb_webview_t *wv;
  mrb_int key;
} dispatch_ctx;

static mrb_value
dispatch_invoke_body(mrb_state *mrb, void *p) {
  mrb_value proc = *(mrb_value *)p;
  return mrb_funcall_with_block(mrb, proc, mrb_intern_lit(mrb, "call"),
                                0, NULL, mrb_nil_value());
}

static void
dispatch_callback(webview_t w, void *arg) {
  (void)w;
  dispatch_ctx *ctx = (dispatch_ctx *)arg;
  mrb_webview_t *wv = ctx->wv;
  mrb_state *mrb = wv->mrb;
  mrb_value self = wv->self;

  int ai = mrb_gc_arena_save(mrb);

  mrb_value dh = DISPATCH_HASH(mrb, self);
  mrb_value key = mrb_int_value(mrb, ctx->key);
  mrb_value proc = mrb_hash_get(mrb, dh, key);

  if (!mrb_nil_p(proc)) {
    /* Errors in dispatch blocks are silently swallowed: there's no caller
     * to propagate them to. mrb_protect_error captures the exception (and
     * clears mrb->exc) so the rest of the run loop is unaffected. */
    mrb_bool err = FALSE;
    mrb_protect_error(mrb, dispatch_invoke_body, &proc, &err);
  }

  mrb_hash_delete_key(mrb, dh, key);
  mrb_free(mrb, ctx);
  mrb_gc_arena_restore(mrb, ai);
}

/* ------------------------------------------------------------------------- */
/* Free function                                                             */
/* ------------------------------------------------------------------------- */

static void
mrb_webview_free(mrb_state *mrb, void *p) {
  mrb_webview_t *wv = (mrb_webview_t *)p;
  if (wv) {
    if (wv->handle) {
      webview_destroy(wv->handle);
      wv->handle = NULL;
    }
    mrb_free(mrb, wv);
  }
}

/* ------------------------------------------------------------------------- */
/* initialize / destroy                                                      */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_initialize(mrb_state *mrb, mrb_value self) {
  mrb_bool debug = FALSE;
  mrb_value window_handle = mrb_nil_value();
  mrb_get_args(mrb, "|bo", &debug, &window_handle);

  mrb_webview_t *wv = (mrb_webview_t *)DATA_PTR(self);
  if (wv) {
    if (wv->handle) {
      webview_destroy(wv->handle);
      wv->handle = NULL;
    }
  } else {
    wv = (mrb_webview_t *)mrb_malloc(mrb, sizeof(mrb_webview_t));
    wv->handle = NULL;
  }
  wv->mrb = mrb;
  wv->self = self;
  mrb_data_init(self, wv, &mrb_webview_data_type);

  void *win = NULL;
  if (mrb_integer_p(window_handle)) {
    win = (void *)(uintptr_t)mrb_integer(window_handle);
  } else if (!mrb_nil_p(window_handle)) {
    mrb_raise(mrb, E_TYPE_ERROR, "window must be nil or an integer (native handle)");
  }

  wv->handle = webview_create(debug ? 1 : 0, win);
  if (!wv->handle) {
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_class_get(mrb, "Webview"), "Error"),
              "failed to create webview (missing system dependencies?)");
  }
  return self;
}

static void
free_all_binding_ctxs(mrb_state *mrb, mrb_value self) {
  mrb_value ch = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@_binding_ctxs"));
  if (!mrb_hash_p(ch)) return;
  mrb_value keys = mrb_hash_keys(mrb, ch);
  for (mrb_int i = 0; i < RARRAY_LEN(keys); i++) {
    mrb_value v = mrb_hash_get(mrb, ch, RARRAY_PTR(keys)[i]);
    if (mrb_cptr_p(v)) {
      mrb_free(mrb, mrb_cptr(v));
    }
  }
  mrb_iv_remove(mrb, self, mrb_intern_lit(mrb, "@_binding_ctxs"));
}

static mrb_value
mrb_webview_m_destroy(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = (mrb_webview_t *)DATA_PTR(self);
  if (wv && wv->handle) {
    free_all_binding_ctxs(mrb, self);
    webview_destroy(wv->handle);
    wv->handle = NULL;
  }
  return mrb_nil_value();
}

static mrb_value
mrb_webview_m_destroyed_p(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = (mrb_webview_t *)DATA_PTR(self);
  return mrb_bool_value(!wv || !wv->handle);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_run(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  mrb_webview_check(mrb, webview_run(wv->handle));
  return self;
}

static mrb_value
mrb_webview_m_terminate(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  mrb_webview_check(mrb, webview_terminate(wv->handle));
  return self;
}

/* ------------------------------------------------------------------------- */
/* Window manipulation                                                       */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_set_title(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *title;
  mrb_get_args(mrb, "z", &title);
  mrb_webview_check(mrb, webview_set_title(wv->handle, title));
  return self;
}

static mrb_value
mrb_webview_m_set_size(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  mrb_int w, h;
  mrb_int hint = WEBVIEW_HINT_NONE;
  mrb_get_args(mrb, "ii|i", &w, &h, &hint);
  mrb_webview_check(mrb, webview_set_size(wv->handle, (int)w, (int)h, (webview_hint_t)hint));
  return self;
}

static mrb_value
mrb_webview_m_navigate(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *url;
  mrb_get_args(mrb, "z", &url);
  mrb_webview_check(mrb, webview_navigate(wv->handle, url));
  return self;
}

static mrb_value
mrb_webview_m_set_html(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *html;
  mrb_get_args(mrb, "z", &html);
  mrb_webview_check(mrb, webview_set_html(wv->handle, html));
  return self;
}

static mrb_value
mrb_webview_m_init_js(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *js;
  mrb_get_args(mrb, "z", &js);
  mrb_webview_check(mrb, webview_init(wv->handle, js));
  return self;
}

static mrb_value
mrb_webview_m_eval(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *js;
  mrb_get_args(mrb, "z", &js);
  mrb_webview_check(mrb, webview_eval(wv->handle, js));
  return self;
}

/* ------------------------------------------------------------------------- */
/* Native handles                                                            */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_window_handle(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  void *p = webview_get_window(wv->handle);
  if (!p) return mrb_nil_value();
  return mrb_int_value(mrb, (mrb_int)(uintptr_t)p);
}

static mrb_value
mrb_webview_m_native_handle(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  mrb_int kind = WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW;
  mrb_get_args(mrb, "|i", &kind);
  void *p = webview_get_native_handle(wv->handle, (webview_native_handle_kind_t)kind);
  if (!p) return mrb_nil_value();
  return mrb_int_value(mrb, (mrb_int)(uintptr_t)p);
}

/* ------------------------------------------------------------------------- */
/* bind / unbind / return                                                    */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_bind(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *name;
  mrb_value blk = mrb_nil_value();
  mrb_get_args(mrb, "z&", &name, &blk);
  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bind requires a block");
  }

  mrb_value bh = BINDINGS_HASH(mrb, self);
  mrb_sym name_sym = mrb_intern_cstr(mrb, name);
  mrb_value name_v = mrb_symbol_value(name_sym);

  mrb_value ch = CTXS_HASH(mrb, self);
  mrb_value existing = mrb_hash_get(mrb, ch, name_v);
  binding_ctx *ctx;
  mrb_bool fresh = FALSE;
  if (mrb_cptr_p(existing)) {
    ctx = (binding_ctx *)mrb_cptr(existing);
  } else {
    ctx = (binding_ctx *)mrb_malloc(mrb, sizeof(binding_ctx));
    ctx->wv = wv;
    ctx->name_sym = name_sym;
    fresh = TRUE;
  }

  webview_error_t err = webview_bind(wv->handle, name, binding_callback, ctx);
  if (err != WEBVIEW_ERROR_OK) {
    if (fresh) mrb_free(mrb, ctx);
    mrb_webview_check(mrb, err);
  }
  if (fresh) {
    mrb_hash_set(mrb, ch, name_v, mrb_cptr_value(mrb, ctx));
  }
  mrb_hash_set(mrb, bh, name_v, blk);
  return self;
}

static mrb_value
mrb_webview_m_unbind(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *name;
  mrb_get_args(mrb, "z", &name);

  mrb_webview_check(mrb, webview_unbind(wv->handle, name));

  mrb_sym name_sym = mrb_intern_cstr(mrb, name);
  mrb_value name_v = mrb_symbol_value(name_sym);
  mrb_hash_delete_key(mrb, BINDINGS_HASH(mrb, self), name_v);

  mrb_value ch = CTXS_HASH(mrb, self);
  mrb_value v = mrb_hash_get(mrb, ch, name_v);
  if (mrb_cptr_p(v)) {
    mrb_free(mrb, mrb_cptr(v));
    mrb_hash_delete_key(mrb, ch, name_v);
  }
  return self;
}

static mrb_value
mrb_webview_m_return(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  const char *id, *result;
  mrb_int status;
  mrb_get_args(mrb, "ziz", &id, &status, &result);
  mrb_webview_check(mrb, webview_return(wv->handle, id, (int)status, result));
  return self;
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_m_dispatch(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = mrb_webview_get(mrb, self);
  mrb_value blk = mrb_nil_value();
  mrb_get_args(mrb, "&", &blk);
  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "dispatch requires a block");
  }

  mrb_value dh = DISPATCH_HASH(mrb, self);

  /* monotonic key */
  mrb_value counter_v = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@_dispatch_counter"));
  mrb_int counter = mrb_integer_p(counter_v) ? mrb_integer(counter_v) : 0;
  counter++;
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@_dispatch_counter"), mrb_int_value(mrb, counter));
  mrb_value key = mrb_int_value(mrb, counter);

  dispatch_ctx *ctx = (dispatch_ctx *)mrb_malloc(mrb, sizeof(dispatch_ctx));
  ctx->wv = wv;
  ctx->key = counter;

  mrb_hash_set(mrb, dh, key, blk);

  webview_error_t err = webview_dispatch(wv->handle, dispatch_callback, ctx);
  if (err != WEBVIEW_ERROR_OK) {
    mrb_hash_delete_key(mrb, dh, key);
    mrb_free(mrb, ctx);
    mrb_webview_check(mrb, err);
  }
  return self;
}

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

static mrb_value
mrb_webview_s_version(mrb_state *mrb, mrb_value self) {
  (void)self;
  const webview_version_info_t *info = webview_version();
  mrb_value h = mrb_hash_new(mrb);
  mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_lit(mrb, "major")),
               mrb_int_value(mrb, info->version.major));
  mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_lit(mrb, "minor")),
               mrb_int_value(mrb, info->version.minor));
  mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_lit(mrb, "patch")),
               mrb_int_value(mrb, info->version.patch));
  mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_lit(mrb, "version")),
               mrb_str_new_cstr(mrb, info->version_number));
  mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_lit(mrb, "pre_release")),
               mrb_str_new_cstr(mrb, info->pre_release));
  mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_lit(mrb, "build_metadata")),
               mrb_str_new_cstr(mrb, info->build_metadata));
  return h;
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */
MRB_BEGIN_DECL
void
mrb_mruby_webview_gem_init(mrb_state *mrb) {
  struct RClass *cls = mrb_define_class(mrb, "Webview", mrb->object_class);
  MRB_SET_INSTANCE_TT(cls, MRB_TT_DATA);

  struct RClass *err = mrb_define_class_under(mrb, cls, "Error", E_RUNTIME_ERROR);
  mrb_define_class_under(mrb, cls, "MissingDependencyError", err);
  mrb_define_class_under(mrb, cls, "CanceledError",          err);
  mrb_define_class_under(mrb, cls, "InvalidStateError",      err);
  mrb_define_class_under(mrb, cls, "InvalidArgumentError",   err);
  mrb_define_class_under(mrb, cls, "DuplicateError",         err);
  mrb_define_class_under(mrb, cls, "NotFoundError",          err);
  mrb_define_class_under(mrb, cls, "DestroyedError",         err);

  mrb_define_method(mrb, cls, "initialize",   mrb_webview_m_initialize,   MRB_ARGS_OPT(2));
  mrb_define_method(mrb, cls, "destroy",      mrb_webview_m_destroy,      MRB_ARGS_NONE());
  mrb_define_method(mrb, cls, "destroyed?",   mrb_webview_m_destroyed_p,  MRB_ARGS_NONE());

  mrb_define_method(mrb, cls, "run",          mrb_webview_m_run,          MRB_ARGS_NONE());
  mrb_define_method(mrb, cls, "terminate",    mrb_webview_m_terminate,    MRB_ARGS_NONE());

  mrb_define_method(mrb, cls, "title=",       mrb_webview_m_set_title,    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "set_title",    mrb_webview_m_set_title,    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "set_size",     mrb_webview_m_set_size,     MRB_ARGS_ARG(2,1));
  mrb_define_method(mrb, cls, "navigate",     mrb_webview_m_navigate,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "url=",         mrb_webview_m_navigate,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "set_html",     mrb_webview_m_set_html,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "html=",        mrb_webview_m_set_html,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "init_script",  mrb_webview_m_init_js,      MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "eval_script",  mrb_webview_m_eval,         MRB_ARGS_REQ(1));

  mrb_define_method(mrb, cls, "window_handle",mrb_webview_m_window_handle,MRB_ARGS_NONE());
  mrb_define_method(mrb, cls, "native_handle",mrb_webview_m_native_handle,MRB_ARGS_OPT(1));

  mrb_define_method(mrb, cls, "_bind_native", mrb_webview_m_bind,         MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
  mrb_define_method(mrb, cls, "unbind",       mrb_webview_m_unbind,       MRB_ARGS_REQ(1));
  mrb_define_method(mrb, cls, "return_result",mrb_webview_m_return,       MRB_ARGS_REQ(3));

  mrb_define_method(mrb, cls, "dispatch",     mrb_webview_m_dispatch,     MRB_ARGS_BLOCK());

  mrb_define_class_method(mrb, cls, "version", mrb_webview_s_version,     MRB_ARGS_NONE());

  mrb_define_const(mrb, cls, "HINT_NONE",  mrb_int_value(mrb, WEBVIEW_HINT_NONE));
  mrb_define_const(mrb, cls, "HINT_MIN",   mrb_int_value(mrb, WEBVIEW_HINT_MIN));
  mrb_define_const(mrb, cls, "HINT_MAX",   mrb_int_value(mrb, WEBVIEW_HINT_MAX));
  mrb_define_const(mrb, cls, "HINT_FIXED", mrb_int_value(mrb, WEBVIEW_HINT_FIXED));

  mrb_define_const(mrb, cls, "NATIVE_HANDLE_UI_WINDOW",
                   mrb_int_value(mrb, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW));
  mrb_define_const(mrb, cls, "NATIVE_HANDLE_UI_WIDGET",
                   mrb_int_value(mrb, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET));
  mrb_define_const(mrb, cls, "NATIVE_HANDLE_BROWSER_CONTROLLER",
                   mrb_int_value(mrb, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
}

void
mrb_mruby_webview_gem_final(mrb_state *mrb) {
  (void)mrb;
}
MRB_END_DECL