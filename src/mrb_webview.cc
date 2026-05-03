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
 * project (see mrbgem.rake). mrbgem.rake also adds WEBVIEW_HEADER (so
 * including the header here only emits declarations) and WEBVIEW_STATIC
 * (so those declarations have plain `extern` linkage that resolves at
 * link time against the static library). */
#include <webview/webview.h>
#include <mruby/fast_json.h>

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
    mrb_raise(mrb,
      mrb_class_get_under_id(mrb, mrb_class_get_id(mrb, MRB_SYM(Webview)), MRB_SYM(DestroyedError)),
      "Webview instance has been destroyed");
  }
  return wv;
}

/* ------------------------------------------------------------------------- */
/* Error helpers                                                             */
/* ------------------------------------------------------------------------- */

static struct RClass *
mrb_webview_error_class(mrb_state *mrb, webview_error_t err) {
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
  struct RClass *json = mrb_module_get_id(mrb, MRB_SYM(JSON));
  return mrb_funcall_id(mrb, mrb_obj_value(json), MRB_SYM(parse), 1, mrb_str_new_cstr(mrb, src));
}

static mrb_value
mrb_webview_json_dump(mrb_state *mrb, mrb_value v) {
  return mrb_json_dump(mrb, v);
}

/* ------------------------------------------------------------------------- */
/* Internal storage on the Ruby instance                                     */
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

#define BINDINGS_HASH(mrb, self) iv_hash(mrb, self, MRB_IVSYM(_bindings))
#define CTXS_HASH(mrb, self)     iv_hash(mrb, self, MRB_IVSYM(_binding_ctxs))
#define DISPATCH_HASH(mrb, self) iv_hash(mrb, self, MRB_IVSYM(_dispatch_procs))

/* ------------------------------------------------------------------------- */
/* Bind callback machinery                                                   */
/* ------------------------------------------------------------------------- */

typedef struct binding_ctx {
  mrb_webview_t *wv;
  mrb_sym name_sym;
} binding_ctx;

static void
binding_ctx_dfree(mrb_state *mrb, void *p) {
  if (p) mrb_free(mrb, p);
}

static const struct mrb_data_type binding_ctx_type = {
  "Webview::BindingCtx", binding_ctx_dfree
};

/* Dedicated internal classes for the two Data wrappers below. They live
 * under Webview, have their instance type tag set to MRB_TT_CDATA, and have
 * their public `new` undefined (only the C extension creates instances).
 * Using a real class instead of mrb->object_class is the convention for
 * Data_Make_Struct: the resulting RData carries the right class, not the
 * generic Object, which keeps introspection sane and avoids any subtle
 * cross-talk with mruby's own Object handling. */
static struct RClass *
binding_ctx_class(mrb_state *mrb) {
  return mrb_class_get_under_id(mrb,
    mrb_class_get_id(mrb, MRB_SYM(Webview)), MRB_SYM(BindingCtx));
}

static binding_ctx *
binding_ctx_new(mrb_state *mrb, mrb_webview_t *wv, mrb_sym name_sym,
                mrb_value *out_data) {
  binding_ctx *ctx;
  struct RData *data;
  Data_Make_Struct(mrb, binding_ctx_class(mrb), binding_ctx,
                   &binding_ctx_type, ctx, data);
  ctx->wv = wv;
  ctx->name_sym = name_sym;
  *out_data = mrb_obj_value(data);
  return ctx;
}

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
  return mrb_funcall_with_block(mrb, s->proc, MRB_SYM(call),
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
    mrb_value msg = mrb_funcall_id(mrb, parsed, MRB_SYM(message), 0);
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
    mrb_value msg  = mrb_funcall_id(mrb, result, MRB_SYM(message), 0);
    mrb_value cls  = mrb_funcall_id(mrb, result, MRB_SYM(class), 0);
    mrb_value name = mrb_funcall_id(mrb, cls, MRB_SYM(name), 0);
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

static void
dispatch_ctx_dfree(mrb_state *mrb, void *p) {
  if (p) mrb_free(mrb, p);
}

static const struct mrb_data_type dispatch_ctx_type = {
  "Webview::DispatchCtx", dispatch_ctx_dfree
};

static struct RClass *
dispatch_ctx_class(mrb_state *mrb) {
  return mrb_class_get_under_id(mrb,
    mrb_class_get_id(mrb, MRB_SYM(Webview)), MRB_SYM(DispatchCtx));
}

static dispatch_ctx *
dispatch_ctx_new(mrb_state *mrb, mrb_webview_t *wv, mrb_int key,
                 mrb_value *out_data) {
  dispatch_ctx *ctx;
  struct RData *data;
  Data_Make_Struct(mrb, dispatch_ctx_class(mrb), dispatch_ctx,
                   &dispatch_ctx_type, ctx, data);
  ctx->wv = wv;
  ctx->key = key;
  *out_data = mrb_obj_value(data);
  return ctx;
}

static mrb_value
dispatch_invoke_body(mrb_state *mrb, void *p) {
  mrb_value proc = *(mrb_value *)p;
  return mrb_funcall_with_block(mrb, proc, MRB_SYM(call),
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
  /* The hash entry is a [proc, ctx_data] pair: the proc keeps the user's
   * block alive, the ctx_data Data wrapper keeps `ctx` itself alive. */
  mrb_value pair = mrb_hash_get(mrb, dh, key);

  if (mrb_array_p(pair) && RARRAY_LEN(pair) >= 1) {
    mrb_value proc = RARRAY_PTR(pair)[0];
    if (!mrb_nil_p(proc)) {
      /* Errors in dispatch blocks are silently swallowed: there's no caller
       * to propagate them to. mrb_protect_error captures the exception (and
       * clears mrb->exc) so the rest of the run loop is unaffected. */
      mrb_bool err = FALSE;
      mrb_protect_error(mrb, dispatch_invoke_body, &proc, &err);
    }
  }

  /* Drop the last reference to the dispatch_ctx Data wrapper. GC will
   * call dispatch_ctx_dfree to release the struct on its next sweep. */
  mrb_hash_delete_key(mrb, dh, key);
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

  void *win = NULL;
  if (mrb_integer_p(window_handle)) {
    win = (void *)(uintptr_t)mrb_integer(window_handle);
  } else if (!mrb_nil_p(window_handle)) {
    mrb_raise(mrb, E_TYPE_ERROR, "window must be nil or an integer (native handle)");
  }

  /* Allocate the mrb_webview_t struct via Data_Make_Struct, then move
   * ownership onto `self` (the outer Webview Data instance produced by
   * Class#new). The throwaway tmp RData is disarmed and reclaimed by GC.
   * This keeps Webview.new -> #initialize working — kwargs forwarding
   * from the mrblib initialize override stays intact — without any
   * direct mrb_malloc on our side. */
  mrb_webview_t *wv;
  struct RData *tmp;
  Data_Make_Struct(mrb, mrb_obj_class(mrb, self), mrb_webview_t,
                   &mrb_webview_data_type, wv, tmp);

  /* If initialize is called more than once, free the previous wv. */
  if (DATA_PTR(self) && DATA_TYPE(self) == &mrb_webview_data_type) {
    mrb_webview_free(mrb, DATA_PTR(self));
  }
  DATA_PTR(self)  = wv;
  DATA_TYPE(self) = &mrb_webview_data_type;

  /* Disarm tmp so GC doesn't double-free wv when it collects the tmp
   * RData wrapper. The dfree sees NULL and short-circuits. The DATA_PTR
   * / DATA_TYPE macros expect an mrb_value, but Data_Make_Struct hands
   * us the RData* directly, so we touch its fields here. */
  tmp->data = NULL;
  tmp->type = NULL;

  wv->mrb = mrb;
  wv->self = self;
  wv->handle = webview_create(debug ? 1 : 0, win);
  if (!wv->handle) {
    mrb_raise(mrb,
      mrb_class_get_under_id(mrb, mrb_class_get_id(mrb, MRB_SYM(Webview)), MRB_SYM(Error)),
      "failed to create webview (missing system dependencies?)");
  }
  return self;
}

static mrb_value
mrb_webview_m_destroy(mrb_state *mrb, mrb_value self) {
  mrb_webview_t *wv = (mrb_webview_t *)DATA_PTR(self);
  if (wv && wv->handle) {
    /* webview_destroy unbinds every C callback, so the binding_ctx /
     * dispatch_ctx pointers we passed via webview_bind / webview_dispatch
     * are no longer used. Removing the iv hashes drops the last
     * references to their Data wrappers; GC reclaims them on its next
     * sweep and runs the dfree functions to release the structs. */
    webview_destroy(wv->handle);
    wv->handle = NULL;
    mrb_iv_remove(mrb, self, MRB_IVSYM(_bindings));
    mrb_iv_remove(mrb, self, MRB_IVSYM(_binding_ctxs));
    mrb_iv_remove(mrb, self, MRB_IVSYM(_dispatch_procs));
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
  binding_ctx *ctx = (binding_ctx *)mrb_data_check_get_ptr(mrb, existing, &binding_ctx_type);
  mrb_bool fresh = !ctx;
  if (fresh) {
    mrb_value ctx_data;
    ctx = binding_ctx_new(mrb, wv, name_sym, &ctx_data);
    mrb_hash_set(mrb, ch, name_v, ctx_data);
  }

  webview_error_t err = webview_bind(wv->handle, name, binding_callback, ctx);
  if (err != WEBVIEW_ERROR_OK) {
    /* Drop the just-stored Data; GC will reclaim the struct. */
    if (fresh) mrb_hash_delete_key(mrb, ch, name_v);
    mrb_webview_check(mrb, err);
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

  /* GC will free the binding_ctx struct via the Data wrapper once we drop
   * the last reference to it (the entry in @_binding_ctxs). */
  mrb_hash_delete_key(mrb, CTXS_HASH(mrb, self), name_v);
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
  mrb_value counter_v = mrb_iv_get(mrb, self, MRB_IVSYM(_dispatch_counter));
  mrb_int counter = mrb_integer_p(counter_v) ? mrb_integer(counter_v) : 0;
  counter++;
  mrb_iv_set(mrb, self, MRB_IVSYM(_dispatch_counter), mrb_int_value(mrb, counter));
  mrb_value key = mrb_int_value(mrb, counter);

  /* Allocate dispatch_ctx wrapped in a Data wrapper so the GC owns its
   * lifetime. Stash both the user's block and the Data wrapper in the
   * hash under the counter key, so both stay rooted while the dispatch
   * is in flight. The C side gets the inner struct pointer. */
  mrb_value ctx_data;
  dispatch_ctx *ctx = dispatch_ctx_new(mrb, wv, counter, &ctx_data);
  mrb_value pair = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, pair, blk);
  mrb_ary_push(mrb, pair, ctx_data);
  mrb_hash_set(mrb, dh, key, pair);

  webview_error_t err = webview_dispatch(wv->handle, dispatch_callback, ctx);
  if (err != WEBVIEW_ERROR_OK) {
    /* Dropping the entry releases both the proc and the ctx Data to GC. */
    mrb_hash_delete_key(mrb, dh, key);
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

/* ------------------------------------------------------------------------- */
/* LSan suppressions baked into the binary                                   */
/* ------------------------------------------------------------------------- */
/*
 * webview pulls in WebKitGTK / JavaScriptCore / fontconfig / Mesa EGL /
 * Pango / GLib's GObject type system. All of those allocate process-
 * lifetime singletons during their first call (atom-string interns, GType
 * registrations behind pthread_once, EGL contexts, fontconfig caches) and
 * intentionally never free them — there is no public shutdown API for any
 * of them, and webview itself only exposes per-instance webview_destroy
 * (no global teardown). Running the gem under -fsanitize=address therefore
 * always reports those allocations as leaks even when every Webview /
 * BindingCtx / DispatchCtx we own is properly destroyed.
 *
 * LSan looks up the symbol __lsan_default_suppressions at startup and
 * uses whatever string it returns as a frame-matcher suppressions list —
 * exactly the same format as an external suppressions file, but with no
 * need for the user to set LSAN_OPTIONS. Defining it here means
 * `mruby-webview` is leak-clean out of the box under ASan/LSan.
 *
 * The block is only emitted when the translation unit is built with
 * sanitizers, so non-sanitized builds carry zero overhead.
 */
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
extern "C" const char *__lsan_default_suppressions(void);
extern "C" const char *__lsan_default_suppressions(void) {
  return
    /* WebKit / JavaScriptCore: process-lifetime singletons + atom strings. */
    "leak:libwebkit2gtk\n"
    "leak:libwebkitgtk\n"
    "leak:libjavascriptcoregtk\n"
    "leak:WTF::AtomStringImpl\n"
    "leak:WTF::StringImpl\n"
    "leak:JSC::JSGlobalObject\n"
    /* fontconfig: pattern / language-set / config caches. */
    "leak:libfontconfig\n"
    "leak:FcValueSave\n"
    "leak:FcLangSetCopy\n"
    "leak:FcPatternAdd\n"
    "leak:FcFontRenderPrepare\n"
    /* Mesa EGL driver state. */
    "leak:libEGL_mesa\n"
    /* Pango / Cairo font + glyph caches. */
    "leak:libpangocairo\n"
    "leak:libpango\n"
    "leak:libcairo\n"
    /* GdkPixbuf module loader handles. */
    "leak:libgdk_pixbuf\n"
    /* GLib / GIO / GObject one-shot init reached via pthread_once. */
    "leak:libgio-2.0\n"
    "leak:libgobject-2.0\n"
    "leak:libglib-2.0\n"
    "leak:g_type_class_get\n"
    "leak:g_type_register_static\n"
    "leak:g_object_new\n"
    "leak:g_object_new_with_properties\n"
    "leak:g_malloc\n"
    "leak:g_malloc0\n"
    /* Anything reached via pthread_once is a one-shot library init. */
    "leak:pthread_once\n";
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

  /* Internal Data wrapper classes used by binding_ctx_new and
   * dispatch_ctx_new. Their instance type tag is MRB_TT_CDATA so
   * Data_Make_Struct produces RData objects of the right shape, and their
   * `new` is undefined so Ruby code can't construct them directly. */
  struct RClass *bcls = mrb_define_class_under_id(mrb, cls, MRB_SYM(BindingCtx),  mrb->object_class);
  struct RClass *dcls = mrb_define_class_under_id(mrb, cls, MRB_SYM(DispatchCtx), mrb->object_class);
  MRB_SET_INSTANCE_TT(bcls, MRB_TT_CDATA);
  MRB_SET_INSTANCE_TT(dcls, MRB_TT_CDATA);
  mrb_undef_class_method_id(mrb, bcls, MRB_SYM(new));
  mrb_undef_class_method_id(mrb, dcls, MRB_SYM(new));

  struct RClass *err = mrb_define_class_under(mrb, cls, "Error", E_RUNTIME_ERROR);
  mrb_define_class_under(mrb, cls, "MissingDependencyError", err);
  mrb_define_class_under(mrb, cls, "CanceledError",          err);
  mrb_define_class_under(mrb, cls, "InvalidStateError",      err);
  mrb_define_class_under(mrb, cls, "InvalidArgumentError",   err);
  mrb_define_class_under(mrb, cls, "DuplicateError",         err);
  mrb_define_class_under(mrb, cls, "NotFoundError",          err);
  mrb_define_class_under(mrb, cls, "DestroyedError",         err);

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
  mrb_define_method_id(mrb, cls, MRB_SYM(init_script),   mrb_webview_m_init_js,       MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(eval_script),   mrb_webview_m_eval,          MRB_ARGS_REQ(1));

  mrb_define_method_id(mrb, cls, MRB_SYM(window_handle), mrb_webview_m_window_handle, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, cls, MRB_SYM(native_handle), mrb_webview_m_native_handle, MRB_ARGS_OPT(1));

  mrb_define_method_id(mrb, cls, MRB_SYM(_bind_native),  mrb_webview_m_bind,          MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
  mrb_define_method_id(mrb, cls, MRB_SYM(unbind),        mrb_webview_m_unbind,        MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, cls, MRB_SYM(return_result), mrb_webview_m_return,        MRB_ARGS_REQ(3));

  mrb_define_method_id(mrb, cls, MRB_SYM(dispatch),      mrb_webview_m_dispatch,      MRB_ARGS_BLOCK());

  mrb_define_class_method_id(mrb, cls, MRB_SYM(version), mrb_webview_s_version,       MRB_ARGS_NONE());

  mrb_define_const_id(mrb, cls, MRB_SYM(HINT_NONE),  mrb_int_value(mrb, WEBVIEW_HINT_NONE));
  mrb_define_const_id(mrb, cls, MRB_SYM(HINT_MIN),   mrb_int_value(mrb, WEBVIEW_HINT_MIN));
  mrb_define_const_id(mrb, cls, MRB_SYM(HINT_MAX),   mrb_int_value(mrb, WEBVIEW_HINT_MAX));
  mrb_define_const_id(mrb, cls, MRB_SYM(HINT_FIXED), mrb_int_value(mrb, WEBVIEW_HINT_FIXED));

  mrb_define_const_id(mrb, cls, MRB_SYM(NATIVE_HANDLE_UI_WINDOW),
                      mrb_int_value(mrb, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW));
  mrb_define_const_id(mrb, cls, MRB_SYM(NATIVE_HANDLE_UI_WIDGET),
                      mrb_int_value(mrb, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET));
  mrb_define_const_id(mrb, cls, MRB_SYM(NATIVE_HANDLE_BROWSER_CONTROLLER),
                      mrb_int_value(mrb, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
}

void
mrb_mruby_webview_gem_final(mrb_state *mrb) {
  (void)mrb;
}
MRB_END_DECL