# frozen_string_literal: true
#
# mruby-webview: Ruby-level helpers on top of the Hypha module.
#
# Hypha itself (the C side) provides Hypha.run, Hypha.bind, Hypha.html=,
# Hypha.dispatch, etc. This file adds:
#
#   - Hypha.html_router(name): a <script> string that wires
#     rb-{get,post,put,patch,delete} attributes to a Ruby-bound function.
#
#   - A Ruby-level Hypha.bind wrapper that, in addition to registering the
#     binding, injects a small JS shim so JavaScript callers see real Error
#     objects when the Ruby block raises (rather than the {name, message,
#     backtrace} object webview's native bind machinery resolves with).
#
# Both are convenience layers over the C API; Hypha works without them.
CBOR.register_tag(50000) do
  encode Exception do |e|
    [e.class, e.message, e.backtrace]
  end
  decode Array do |a|
    exc = a[0].new(a[1])
    exc.set_backtrace(a[2]) if a[2]
    exc
  end
end

CBOR.register_tag(50001) do
  encode Proc do |p|
    p.to_irep
  end
  decode String do |bytes|
    Proc.from_irep(bytes)
  end
end

module Hypha
  class << self
    # Returns a <script> block that wires rb-{get,post,put,patch,delete}
    # attributes in the page to a Ruby-bound function. Drop it into <head>.
    #
    #   Hypha.run do |h|
    #     h.bind(:route) { |method, path, params| render_page(method, path, params) }
    #     h.html = "<head>#{Hypha.html_router(:route)}</head>..."
    #   end
    #
    # The bind name must be a plain ASCII JS identifier.
    def html_router(bind_name)
      name = bind_name.to_s
      raise ArgumentError, "router bind name must be a JS identifier" \
        unless js_identifier?(name)
      ROUTER_TEMPLATE.render('route_fn' => name)
    end

    # Wrap C-level Hypha.bind to also inject the JS error shim. Calling
    # Hypha.bind from Ruby goes through here; bind name -> proc registration
    # is delegated to the C method (aliased to _native_bind below), and we
    # follow up with init() to install the shim that promotes resolved-with-
    # error-object rejections into real Error throws on the JS side.
    #
    # Re-binding the same name replaces the previous block (C-side handles
    # this) and is a no-op shim-wise (the shim itself is idempotent thanks
    # to the __mwebshim marker).
    def bind(name, callable = nil, &block)
      proc_obj = block || callable
      raise ArgumentError, "bind requires a block or callable" unless proc_obj
      proc_obj = proc_obj.to_proc unless proc_obj.is_a?(Proc)

      _native_bind(name, &proc_obj)
      _install_error_shim(name)
      self
    end

    private

    def js_identifier?(s)
      return false if s.empty?
      s.each_char.with_index do |c, i|
        ok = (c >= 'a' && c <= 'z') ||
             (c >= 'A' && c <= 'Z') ||
             c == '_' || c == '$' ||
             (i > 0 && c >= '0' && c <= '9')
        return false unless ok
      end
      true
    end

    # webview rejects promises with a plain {name:, message:, backtrace:}
    # object when the Ruby block raises. Promote it to a real Error so JS
    # callers get a readable stack and can use instanceof / .message.
    #
    # Idempotent: re-binding the same name (or any other reason this script
    # runs twice on a page) won't stack wrappers. Real Error objects bypass
    # promotion entirely thanks to the Array.isArray(e.backtrace) guard.
    def _install_error_shim(name)
      js_name = name.to_s.inspect
      init(<<~JS)
        (function () {
          var fn = window[#{js_name}];
          if (typeof fn !== 'function' || fn.__mwebshim) return;
          var wrapped = function () {
            return fn.apply(this, arguments).catch(function (e) {
              if (e && typeof e === 'object' &&
                  typeof e.message === 'string' &&
                  Array.isArray(e.backtrace)) {
                var err = new Error(e.message);
                err.name = typeof e.name === 'string' ? e.name : 'Error';
                if (e.backtrace.length > 0) {
                  err.stack = err.name + ': ' + err.message + '\\n' +
                              e.backtrace.map(function(l) { return '    ' + l; }).join('\\n');
                }
                throw err;
              }
              throw e;
            });
          };
          wrapped.__mwebshim = true;
          window[#{js_name}] = wrapped;
        })();
      JS
    end

    # rb-{verb} attribute router. The bind name is templated in via Mustache
    # so users can pick whatever they want for the Ruby-side function.
    # Compiled once at load.
    ROUTER_TEMPLATE = Mustache::Template.compile(<<~'JS')
    <script>
      (function () {
        var ROUTE = window.{{route_fn}};
        var SEL = '[rb-get],[rb-post],[rb-put],[rb-patch],[rb-delete]';
        var VERBS = ['get', 'post', 'put', 'patch', 'delete'];

        function methodAndPath(elt) {
          for (var i = 0; i < VERBS.length; i++) {
            if (elt.hasAttribute('rb-' + VERBS[i])) {
              return { method: VERBS[i].toUpperCase(),
                       path: elt.getAttribute('rb-' + VERBS[i]) };
            }
          }
        }

        function parseDuration(s) {
          var m = /^(\d+(?:\.\d+)?)(ms|s)?$/.exec(s || '');
          if (!m) return 0;
          var n = parseFloat(m[1]);
          return m[2] === 's' ? n * 1000 : n;
        }

        // 'input changed delay:200ms, click'
        //   -> [{event:'input',changed:true,delay:200},{event:'click'}]
        function parseTriggers(spec, defaultEv) {
          if (!spec) return [{ event: defaultEv }];
          return spec.split(/\s*,\s*/).map(function (chunk) {
            var parts = chunk.trim().split(/\s+/);
            var t = { event: parts[0] };
            for (var i = 1; i < parts.length; i++) {
              var p = parts[i], c = p.indexOf(':');
              if (c >= 0) {
                var key = p.slice(0, c), val = p.slice(c + 1);
                if (key === 'delay') t.delay = parseDuration(val);
              } else {
                t[p] = true; // 'changed', 'once', ...
              }
            }
            return t;
          });
        }

        function dispatch(elt, e) {
          if (e && e.preventDefault) e.preventDefault();
          var mp = methodAndPath(elt); if (!mp) return;

          var params = {};

          // 1. Lone form-control: contribute its own name/value first so an
          //    explicit rb-vals can still override it.
          if (elt.tagName !== 'FORM' && elt.name && 'value' in elt) {
            params[elt.name] = elt.value;
          }
          // 2. FORM: harvest every named field.
          if (elt.tagName === 'FORM') {
            new FormData(elt).forEach(function (v, k) { params[k] = v; });
          }
          // 3. rb-vals JSON wins over both.
          var raw = elt.getAttribute('rb-vals');
          if (raw) try { Object.assign(params, JSON.parse(raw)); } catch (_) {}

          var targetSel = elt.getAttribute('rb-target');
          var target    = targetSel ? document.querySelector(targetSel) : elt;
          var swap      = elt.getAttribute('rb-swap') || 'innerHTML';
          var indSel    = elt.getAttribute('rb-indicator');
          var ind       = indSel ? document.querySelector(indSel) : null;

          if (ind) ind.classList.add('busy');
          elt.querySelectorAll('button').forEach(function (b) { b.disabled = true; });

          ROUTE(mp.method, mp.path, params).then(function (html) {
              if (swap === 'delete')          { target.remove(); }
              else if (swap === 'outerHTML')  { target.outerHTML = html; }
              else if (swap === 'innerHTML' || !swap) { target.innerHTML = html; }
              else                            { target.insertAdjacentHTML(swap, html); }
            }).catch(function (err) {
            target.innerHTML = "<div class='error'>" +
              (err && err.message ? err.message : String(err)) + "</div>";
          }).finally(function () {
            if (ind) ind.classList.remove('busy');
            elt.querySelectorAll('button').forEach(function (b) { b.disabled = false; });
          });
        }

        function wire(root) {
          root.querySelectorAll(SEL).forEach(function (elt) {
            if (elt.__rb_wired) return;
            elt.__rb_wired = true;

            var defaultEv =
              elt.tagName === 'FORM' ? 'submit' :
              (elt.tagName === 'INPUT' || elt.tagName === 'TEXTAREA' ||
               elt.tagName === 'SELECT') ? 'change' : 'click';

            var triggers = parseTriggers(elt.getAttribute('rb-trigger'), defaultEv);

            triggers.forEach(function (t) {
              if (t.event === 'load') { dispatch(elt); return; }

              var lastValue = ('value' in elt) ? elt.value : undefined;
              var timer = null;

              elt.addEventListener(t.event, function (e) {
                if (t.changed) {
                  var v = elt.value;
                  if (v === lastValue) return;
                  lastValue = v;
                }
                if (t.delay) {
                  clearTimeout(timer);
                  timer = setTimeout(function () { dispatch(elt, e); }, t.delay);
                } else {
                  dispatch(elt, e);
                }
              });
            });
          });
        }

        document.addEventListener('DOMContentLoaded', function () { wire(document); });
        new MutationObserver(function (muts) {
          muts.forEach(function (m) { m.addedNodes.forEach(function (n) {
            if (n.nodeType === 1) wire(n);
          }); });
        }).observe(document.documentElement, { childList: true, subtree: true });
      })();
    </script>
    JS
  end
end