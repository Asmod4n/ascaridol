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

  # webview rejects promises with a plain {name:, message:} object when the
  # Ruby block raises. Promote it to a real Error so JS callers get a readable
  # stack and can use instanceof / .message normally.
  def _install_error_shim(name)
    js_name = name.to_s.inspect
    init(<<~JS)
      (function () {
        var fn = window[#{js_name}];
        if (typeof fn !== 'function') return;
        window[#{js_name}] = function () {
          return fn.apply(this, arguments).catch(function (e) {
            if (e && typeof e === 'object' && typeof e.message === 'string') {
              var err = new Error(e.message);
              err.name = e.name || 'Error';
              throw err;
            }
            throw e;
          });
        };
      })();
    JS
  end
end
