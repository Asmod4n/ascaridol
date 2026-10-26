# frozen_string_literal: true
#
# mruby-webview: Ruby-level helpers on top of the Ascaridol module.
#
# Ascaridol itself (the C side) provides Ascaridol.run, Ascaridol.bind, Ascaridol.html=,
# Ascaridol.dispatch, etc. This file adds:
#
#   - Ascaridol.htmx(name): htmx 4 and an htmx extension that sends each
#     request to a Ruby-bound function instead of the network.
#
#   - A Ruby-level Ascaridol.bind wrapper that, in addition to registering the
#     binding, injects a small JS shim so JavaScript callers see real Error
#     objects when the Ruby block raises (rather than the {name, message,
#     backtrace} object webview's native bind machinery resolves with).
#
# Both are convenience layers over the C API; Ascaridol works without them.
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

module Ascaridol
  class << self
    # Returns htmx 4 and the extension for <head>. Each hx-get, hx-post,
    # hx-put, hx-patch and hx-delete calls the Ruby function bound under
    # bind_name with (method, path, params). The block returns an HTML
    # String, [status, html] or nil. Without the binding, as over HTTP,
    # the extension leaves htmx to the network.
    #
    #   Ascaridol.run do |h|
    #     h.bind(:route) { |method, path, params| render_page(method, path, params) }
    #     h.html = "<head>#{Ascaridol.htmx(:route)}</head>..."
    #   end
    #
    # The bind name must be a plain ASCII JS identifier.
    #
    # prefix is htmx.config.prefix, the second prefix that htmx reads
    # beside hx-. The empty default makes htmx read hx- names only;
    # prefix: "data-hx-" gives back the htmx default.
    def htmx(bind_name, prefix: "")
      name = bind_name.to_s
      raise ArgumentError, "htmx bind name must be a JS identifier" \
        unless js_identifier?(name)
      raise ArgumentError, "htmx prefix must hold only a-z, 0-9 and -" \
        unless html_attribute_prefix?(prefix)
      HTMX_CONFIG_TEMPLATE.render({ prefix: prefix }) +
        "<script>#{HTMX_SOURCE}</script>" +
        HTMX_EXTENSION_TEMPLATE.render({ bind_name: name })
    end

    # Wrap C-level Ascaridol.bind to also inject the JS error shim. Calling
    # Ascaridol.bind from Ruby goes through here; bind name -> proc registration
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

    def html_attribute_prefix?(s)
      s.is_a?(String) && s.each_char.all? do |c|
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'
      end
    end

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

    HTMX_CONFIG_TEMPLATE = Mustache::Template.compile(
      %q(<meta name="htmx-config" content='{"history":false,"prefix":"{{prefix}}"}'>))

    HTMX_EXTENSION_TEMPLATE = Mustache::Template.compile(<<~'JS')
    <script>
      (function () {
        var BRIDGE_BASE = 'http://ascaridol.invalid/';
        var NO_BODY = [204, 205, 304];

        function bridge() { return window['{{bind_name}}']; }

        function escapeHtml(s) {
          return String(s).replace(/[&<>"']/g, function (c) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
          });
        }

        function paramsOf(action, body) {
          var url = new URL(action, BRIDGE_BASE);
          var params = Object.fromEntries(url.searchParams);
          if (body) {
            var fields = body instanceof FormData ? body : new URLSearchParams(body);
            for (var [k, v] of fields) {
              if (typeof v !== 'string')
                throw new TypeError('ascaridol: a file cannot be sent to Ruby');
              params[k] = v;
            }
          }
          return { path: url.pathname, params: params };
        }

        function responseOf(result) {
          var status = 200, body = result;
          if (Array.isArray(result)) { status = result[0]; body = result[1]; }
          if (NO_BODY.indexOf(status) >= 0) return new Response(null, { status: status });
          if (body == null) return new Response(null, { status: 204 });
          return new Response(String(body), { status: status, headers: { 'Content-Type': 'text/html' } });
        }

        function errorResponseOf(err) {
          var message = err && err.message ? err.message : String(err);
          return new Response("<div class='error'>" + escapeHtml(message) + "</div>",
                              { status: 500, headers: { 'Content-Type': 'text/html' } });
        }

        function abortOf(signal) {
          return new Promise(function (_, reject) {
            var abort = function () { reject(new DOMException('aborted', 'AbortError')); };
            if (signal.aborted) abort();
            else signal.addEventListener('abort', abort);
          });
        }

        htmx.registerExtension('ascaridol', {
          htmx_config_request: function (elt, detail) {
            if (typeof bridge() !== 'function') return;
            detail.ctx.request.action = new URL(detail.ctx.request.action, BRIDGE_BASE).href;
          },
          htmx_before_request: function (elt, detail) {
            if (typeof bridge() !== 'function') return;
            detail.ctx.fetch = function (action, req) {
              var call;
              try {
                var r = paramsOf(action, req.body);
                call = Promise.resolve(bridge()(req.method, r.path, r.params))
                  .then(responseOf, errorResponseOf);
              } catch (e) {
                call = Promise.resolve(errorResponseOf(e));
              }
              return Promise.race([call, abortOf(req.signal)]);
            };
          }
        });
      })();
    </script>
    JS
  end
end
