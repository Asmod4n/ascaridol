# frozen_string_literal: true
#
# mruby-webview: idiomatic Ruby bindings for webview/webview.

class Webview
  module SizeHint
    NONE  = HINT_NONE
    MIN   = HINT_MIN
    MAX   = HINT_MAX
    FIXED = HINT_FIXED
  end

  HINTS = {
    none:  HINT_NONE,
    min:   HINT_MIN,
    max:   HINT_MAX,
    fixed: HINT_FIXED
  }.freeze

  HANDLES = {
    window:             NATIVE_HANDLE_UI_WINDOW,
    widget:             NATIVE_HANDLE_UI_WIDGET,
    browser_controller: NATIVE_HANDLE_BROWSER_CONTROLLER
  }.freeze

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
      set_size(w, h, _hint_value(hint || :none))
    end
    self.url  = url  if url
    self.html = html if html
  end

  # Set window size with an optional symbolic hint.
  alias _native_set_size set_size
  def set_size(width, height, hint = :none)
    _native_set_size(width, height, _hint_value(hint))
    self
  end

  # Bind a Ruby block to a JavaScript function name.
  #
  # The block receives the deserialized JSON arguments. Its return value is
  # serialized back to JSON for the JavaScript caller. Exceptions are reported
  # to JavaScript as a rejected promise carrying { name:, message: }.
  #
  #   wv.bind("greet") { |name| "Hello, #{name}!" }
  #
  # Calling `bind` again with the same name replaces the previous block.
  def bind(name, callable = nil, &block)
    proc_obj = block || callable
    raise ArgumentError, "bind requires a block or callable" unless proc_obj
    proc_obj = proc_obj.to_proc unless proc_obj.is_a?(Proc)
    _bind_native(name.to_s, &proc_obj)
    self
  end

  # Returns the registered binding names (as Strings).
  def bindings
    h = instance_variable_get(:@_bindings)
    return [] unless h
    h.keys.map(&:to_s)
  end

  # Convenience accessors mirroring the C API.
  def title=(t); set_title(t.to_s); end
  alias eval eval_script
  alias evaluate eval_script
  alias init_js init_script

  # Returns the native handle for the given kind (`:window`, `:widget`,
  # `:browser_controller`) as an Integer pointer.
  def handle(kind = :window)
    native_handle(_handle_kind(kind))
  end

  def to_s
    "#<Webview destroyed=#{destroyed?}>"
  end
  alias inspect to_s

  private

  def _hint_value(hint)
    case hint
    when Symbol then HINTS.fetch(hint) { raise ArgumentError, "unknown size hint: #{hint.inspect}" }
    when Integer then hint
    when nil then HINT_NONE
    else raise ArgumentError, "size hint must be a Symbol or Integer"
    end
  end

  def _handle_kind(kind)
    case kind
    when Symbol then HANDLES.fetch(kind) { raise ArgumentError, "unknown handle kind: #{kind.inspect}" }
    when Integer then kind
    else raise ArgumentError, "handle kind must be a Symbol or Integer"
    end
  end
end
