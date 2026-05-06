# frozen_string_literal: true
#
# mruby-webview: idiomatic Ruby bindings for webview/webview.

class Webview
  class << self
    # Create, configure and run a Webview in one block. The window is destroyed
    # when the block exits, even on exception.
    #
    #   Webview.open(title: "Hello", size: [800, 600]) do |w|
    #     w.html = "<h1>Hi</h1>"
    #   end
    def open(**opts)
      w = new(**opts)
      begin
        yield w if block_given?
        w.run
      ensure
        w.destroy unless w.destroyed?
      end
    end
  end

  alias _native_initialize initialize

  # Webview.new(debug: false, window: nil, title: nil, size: nil, url: nil, html: nil)
  #
  # All keyword arguments are optional. `size` may be `[w, h]` or `[w, h, hint]`,
  # where `hint` is one of `:none`, `:min`, `:max`, `:fixed`.
  def initialize(debug: false, window: nil, title: nil, size: nil, url: nil, html: nil)
    _native_initialize(debug, window)
    self.title = title if title
    if size
      w, h, hint = size
      set_size(w, h, hint)
    end
    self.url  = url  if url
    self.html = html if html
  end

  # Bind a Ruby block to a JavaScript function name.
  #
  # The block receives the deserialized JSON arguments. Its return value is
  # serialized back to JSON for the JavaScript caller. Exceptions are reported
  # to JavaScript as a rejected promise carrying { name:, message: }.
  #
  #   wv.bind(:greet) { |name| "Hello, #{name}!" }
  #
  # Calling `bind` again with the same name replaces the previous block.
  def bind(name, callable = nil, &block)
    proc_obj = block || callable
    raise ArgumentError, "bind requires a block or callable" unless proc_obj
    proc_obj = proc_obj.to_proc unless proc_obj.is_a?(Proc)
    _bind_native(name, &proc_obj)
    _install_error_shim(name)
    self
  end


  # Convenience accessors mirroring the C API.
  def title=(t); set_title(t.to_s); end

  def to_s
    "#<Webview destroyed=#{destroyed?}>"
  end
  alias inspect to_s

  private

  # webview rejects promises with a plain {name:, message:, backtrace:} object
  # when the Ruby block raises. Promote it to a real Error so JS callers get a
  # readable stack and can use instanceof / .message normally.
  #
  # Idempotent: re-binding the same name (or any other reason this script runs
  # twice on a page) won't stack wrappers. Real Error objects bypass promotion
  # entirely thanks to the `Array.isArray(e.backtrace)` guard.
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

  MRUBY_ROUTER_EXT = <<~'JS'
  <script>
    (function () {
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

      function dispatch(elt, e) {
        if (e) e.preventDefault();
        var mp = methodAndPath(elt); if (!mp) return;

        var params = {};
        var raw = elt.getAttribute('rb-vals');
        if (raw) try { Object.assign(params, JSON.parse(raw)); } catch (_) {}
        if (elt.tagName === 'FORM') {
          new FormData(elt).forEach(function (v, k) { params[k] = v; });
        }

        var targetSel = elt.getAttribute('rb-target');
        var target    = targetSel ? document.querySelector(targetSel) : elt;
        var swap      = elt.getAttribute('rb-swap') || 'innerHTML';
        var indSel    = elt.getAttribute('rb-indicator');
        var ind       = indSel ? document.querySelector(indSel) : null;

        if (ind) ind.classList.add('busy');
        elt.querySelectorAll('button').forEach(function (b) { b.disabled = true; });

        window.htmx_route(mp.method, mp.path, params).then(function (html) {
          if (swap === 'outerHTML') target.outerHTML = html;
          else target.innerHTML = html;
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
          var trig = (elt.getAttribute('rb-trigger') ||
                     (elt.tagName === 'FORM' ? 'submit' : 'click')).split(/\s*,\s*/);
          trig.forEach(function (t) {
            var ev = t.trim();
            if (ev === 'load') { dispatch(elt); return; }
            elt.addEventListener(ev, function (e) { dispatch(elt, e); });
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
