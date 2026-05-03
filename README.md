# mruby-webview

Idiomatic mruby bindings for [`webview/webview`](https://github.com/webview/webview)
— a tiny cross-platform library that lets you build desktop GUIs with HTML / CSS / JS,
backed by the system's native browser engine (WebKitGTK on Linux, WebKit on
macOS, WebView2 on Windows).

The bindings are built on webview's **C++ engine class** (`webview::webview`)
directly, with `std::function` lambdas for the bind / dispatch hooks and
`std::string` everywhere a path / URL / script body is passed. The mruby-side
GC owns the C++ instance via [`mruby-c-ext-helpers`](https://github.com/asmod4n/mruby-c-ext-helpers)'
`mrb_cpp_new` / `mrb_cpp_get` / `mrb_cpp_delete`. JSON round-tripping for
bind callbacks goes through [`mruby-fast-json`](https://github.com/asmod4n/mruby-fast-json).

## Installation

Add this gem to your `build_config.rb`:

```ruby
MRuby::Build.new do |conf|
  conf.toolchain :gcc
  conf.cc.defines  << 'MRB_UTF8_STRING'
  conf.cxx.defines << 'MRB_UTF8_STRING'
  conf.gem github: 'asmod4n/mruby-webview', branch: 'main',
           options: { recursive: true }   # pull the webview submodule too
end
```

The upstream `webview/webview` source ships as a git submodule under
`vendor/webview`. If you cloned without `--recurse-submodules`, `mrbgem.rake`
will run `git submodule update --init --recursive vendor/webview` for you the
first time it builds.

To check the gem out manually:

```sh
git clone --recurse-submodules https://github.com/asmod4n/mruby-webview.git
# or, after a plain clone:
git -C mruby-webview submodule update --init --recursive
```

`mrbgem.rake` then:

1. Pulls in `mruby-fast-json` and `mruby-c-ext-helpers` as gem dependencies.
2. Adds `vendor/webview/core/include` to the include path. Webview's full C++
   implementation is compiled directly into the gem's single `.cc`
   translation unit (no `WEBVIEW_HEADER`, no `WEBVIEW_STATIC`, no separate
   `libwebview.a`) so the bindings can use `webview::webview` directly.
3. Detects the platform via the build's toolchain / target triple and adds
   the right link flags — pkg-config'd `gtk` + `webkit2gtk` on Linux/BSD,
   `-framework WebKit -framework Cocoa` on macOS, the WebView2 system libs
   on Windows.
4. Bumps the C++ standard to `-std=c++17` (required by `mruby-c-ext-helpers`;
   webview itself only needs C++14).

Override the webview source location with `MRUBY_WEBVIEW_DIR`. Override the
Linux pkg-config combo with `MRUBY_WEBVIEW_PKG`.

**Build prerequisites:** a C++17 toolchain, `git`, and the platform's
WebView dev packages (see below). No CMake required.

### System dependencies

| Platform | Required packages |
| -------- | ----------------- |
| Linux    | `gtk4` + `webkitgtk-6.0`, or `gtk+-3.0` + `webkit2gtk-4.1`/`4.0` |
| macOS    | Xcode command-line tools (WebKit + Cocoa frameworks) |
| Windows  | WebView2 runtime (preinstalled on recent Windows 10 / 11) |

On Debian / Ubuntu:

```sh
sudo apt install libgtk-4-dev libwebkitgtk-6.0-dev pkg-config git
# or, for older systems:
sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev pkg-config git
```

## Quick start

```ruby
Webview.open(title: 'Demo', size: [640, 480, :fixed]) do |w|
  w.bind('greet') { |name| "Hello, #{name}!" }
  w.html = <<~HTML
    <button onclick="greet('world').then(r => document.body.innerText = r)">Hi</button>
  HTML
end
```

`Webview.open` builds the instance, yields it for configuration, calls `run`,
and guarantees `destroy` is called when the run loop exits — even if your
block raises.

## API

### Constructor

```ruby
Webview.new(debug: false, window: nil, title: nil, size: nil, url: nil, html: nil)
```

* `debug:` — enables the developer console / inspector.
* `window:` — an existing native window handle (Integer pointer) to reuse;
  pass `nil` to let webview create one.
* `title:`, `size:`, `url:`, `html:` — convenience initializers, applied
  after the underlying `webview::webview` is constructed.

`size:` accepts `[w, h]` or `[w, h, hint]`, where `hint` is one of
`:none`, `:min`, `:max`, `:fixed` (or the corresponding `Webview::HINT_*`
integer).

### Block helper

```ruby
Webview.open(**opts) { |w| ... }
```

Creates a webview, yields it for configuration, calls `w.run`, and ensures
`w.destroy` runs when the loop exits.

### Window control

```ruby
w.title = 'My App'             # or w.set_title('My App')
w.set_size(800, 600, :fixed)   # hint may be Symbol or Integer
w.url  = 'https://example.com' # or w.navigate(url)
w.html = '<h1>hello</h1>'      # or w.set_html(html)
w.init_script(js)              # runs on every navigation, before page JS
w.eval_script(js)              # evaluate JS in the current page
w.run                          # blocks until terminate
w.terminate                    # exit run loop
w.destroy                      # release native resources
w.destroyed?                   # true after destroy
```

All of these go through webview's C++ engine class with `std::string`
arguments — strings with embedded NULs are passed verbatim.

### Native handles

```ruby
w.window_handle               # default UI window pointer (Integer or nil)
w.handle(:window)             # same as above
w.handle(:widget)             # browser widget
w.handle(:browser_controller) # platform-specific controller
```

### JS ↔ Ruby bindings

```ruby
w.bind('add') { |a, b| a + b }

w.bind('async_work') do |x|
  raise 'bad input' if x.negative?  # rejects the JS promise
  x * 2
end

w.unbind('add')
```

The block is invoked on the UI thread with the JSON-decoded arguments. Its
return value is JSON-encoded and resolves the JS-side promise. Exceptions
propagate back to JavaScript as a rejected promise carrying
`{ name:, message: }`.

`bind` accepts either a block or any callable (`Proc`, `Method`, etc.).
Re-binding the same name simply replaces the proc.

Internally, each `bind` registers a `std::function<void(std::string,
std::string, void*)>` lambda with webview's C++ engine. The lambda captures
the webview instance pointer plus the binding name; on every JS call it
looks the live block up in the instance's hidden `:bindings` iv table and
calls it. The user's block is GC-rooted there until `unbind` (or `destroy`).

### Dispatching from other threads

```ruby
w.dispatch do
  w.eval_script('console.log("from main thread")')
end
```

The block is queued via webview's `dispatch(std::function<void()>)` and
runs once on the UI thread. Exceptions inside dispatch blocks are
swallowed — there's no caller to propagate them to. The block stays
GC-rooted under the instance's hidden `:dispatch_procs` table until the
dispatch fires.

### Manual `resolve` / return

```ruby
w.return_result(id, status, json_string)
```

Calls webview's `resolve(id, status, json)` directly. Rarely needed —
`bind` handles this for you. Provided for advanced users who want to
register their own low-level binding.

### Version

```ruby
Webview.version
# => { major: 0, minor: 12, patch: 0,
#      version: "0.12.0", pre_release: "", build_metadata: "" }
```

### Constants

```ruby
Webview::HINT_NONE   Webview::HINT_MIN   Webview::HINT_MAX   Webview::HINT_FIXED
Webview::NATIVE_HANDLE_UI_WINDOW
Webview::NATIVE_HANDLE_UI_WIDGET
Webview::NATIVE_HANDLE_BROWSER_CONTROLLER

Webview::HINTS    # => { none: 0, min: 1, max: 2, fixed: 3 }
Webview::HANDLES  # => { window: 0, widget: 1, browser_controller: 2 }
```

### Errors

Every error raised from this gem inherits from `Webview::Error`. The
specific subclass is chosen by the `webview_error_t` returned by the
underlying C++ engine, so the Ruby-side `rescue` clause maps cleanly onto
webview's own error taxonomy:

```
Webview::Error
├─ Webview::MissingDependencyError
├─ Webview::CanceledError
├─ Webview::InvalidStateError
├─ Webview::InvalidArgumentError
├─ Webview::DuplicateError
├─ Webview::NotFoundError
└─ Webview::DestroyedError
```

`Webview::DestroyedError` is raised by every method except `destroy` /
`destroyed?` after the instance has been destroyed.

## Threading model

* `run` blocks the calling thread until `terminate` is called or the user
  closes the window.
* `bind` callbacks run synchronously on the UI thread (the same thread
  that called `run`).
* `dispatch` is the only safe way to call into a `Webview` from another
  thread or a signal handler — the block is enqueued and executed on the
  UI thread on the next event-loop iteration.

## Memory ownership

The C++ `webview::webview` instance is owned by the wrapping `Webview`
Ruby object via `mruby-c-ext-helpers`' `MRB_CPP_DEFINE_TYPE`. When the
Ruby instance is GC'd (or `destroy` is called explicitly), the C++
destructor runs, which tears down the GTK / Cocoa / WebView2 window,
unbinds every binding, and drops the dispatch queue — releasing the
captured `std::function` lambdas and the Ruby blocks they reference.

The internal hidden ivars `:bindings` and `:dispatch_procs` are removed
on `destroy`, dropping the last GC root for any blocks that were still
registered.

## Running with AddressSanitizer / LeakSanitizer

WebKitGTK, JavaScriptCore, fontconfig, Mesa, Pango and GLib's GObject
type system all keep process-lifetime singletons (interned atom strings,
GType registrations behind `pthread_once`, EGL contexts, font config
caches) that are never freed. LSan flags them as leaks at process exit
even when every `Webview` instance is properly destroyed.

The gem ships its own `__lsan_default_suppressions` block, compiled into
the binary only when `__SANITIZE_ADDRESS__` is defined. Build mruby with
`conf.enable_sanitizer "address,undefined"` and run the example — LSan
will pick the suppressions up automatically without needing
`LSAN_OPTIONS`. If a leak survives the suppressions list and the stack
points at this gem or your own code, please file a bug.

## License

MIT — see [LICENSE](LICENSE).
