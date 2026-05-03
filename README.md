# mruby-webview

Idiomatic Ruby bindings for [`webview/webview`](https://github.com/webview/webview)
— a tiny cross-platform library that lets you build desktop GUIs with HTML/CSS/JS,
backed by the system's native browser engine (WebKitGTK on Linux, WebKit on
macOS, WebView2 on Windows).

## Installation

Add this gem to your `build_config.rb`:

```ruby
MRuby::Build.new do |conf|
  conf.toolchain :gcc
  conf.gem github: 'asmod4n/mruby-webview', branch: 'main',
           options: { recursive: true }   # pull the webview submodule too
end
```

The upstream `webview/webview` source is included as a git submodule at
`vendor/webview` (pinned to a known-good tag). If you cloned without
`--recurse-submodules`, `mrbgem.rake` will run
`git submodule update --init --recursive vendor/webview` for you.

To check the gem out manually:

```sh
git clone --recurse-submodules https://github.com/asmod4n/mruby-webview.git
# or, after a plain clone:
git -C mruby-webview submodule update --init --recursive
```

`mrbgem.rake` then:

1. Pulls in [`mruby-fast-json`](https://github.com/asmod4n/mruby-fast-json) for
   the JSON round-trip used by `bind` (and defines `MRB_UTF8_STRING`, which it
   requires).
2. Invokes webview's official **CMake** project to build `libwebview.a`
   (static, position-independent) into `<mruby-build>/mrbgems/mruby-webview/vendor/webview-build/`
   and links the bindings against it. Override the source location with
   `MRUBY_WEBVIEW_DIR`, the cmake binary with `CMAKE`, and parallelism with
   `MRUBY_WEBVIEW_JOBS` or `JOBS`.
3. Detects platform libraries via `pkg-config` (Linux) or platform frameworks
   (macOS/Windows) and adds the right link flags.

**Build prerequisites:** a C++ toolchain, `cmake >= 3.16`, `git`, and the
platform's WebView dev packages (see below).

### System dependencies

| Platform | Required packages |
| -------- | ----------------- |
| Linux    | `gtk4` + `webkitgtk-6.0`, or `gtk+-3.0` + `webkit2gtk-4.1`/`4.0` |
| macOS    | Xcode command-line tools (WebKit + Cocoa frameworks) |
| Windows  | WebView2 runtime (preinstalled on recent Windows 10/11) |

On Debian/Ubuntu:

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

## API

### Constructor

```ruby
Webview.new(debug: false, window: nil, title: nil, size: nil, url: nil, html: nil)
```

* `debug:` enables the developer console / inspector.
* `window:` is an existing native window handle (Integer pointer) to reuse;
  pass `nil` to let webview create one.
* `title:`, `size:`, `url:`, `html:` are convenience initializers.

`size:` accepts `[w, h]` or `[w, h, hint]`, where `hint` is one of `:none`,
`:min`, `:max`, `:fixed`.

### Block helper

```ruby
Webview.open(**opts) { |w| ... }
```

Creates a webview, yields it for configuration, runs the event loop and
guarantees `destroy` is called on exit (even on exception).

### Window control

```ruby
w.title = 'My App'           # or w.set_title('My App')
w.set_size(800, 600, :fixed) # hint may be Symbol or Integer
w.url  = 'https://example.com'
w.html = '<h1>hello</h1>'
w.init_script(js)            # script run on every navigation, before page JS
w.eval_script(js)            # evaluate JS in the current page
w.run                        # blocks until terminate
w.terminate                  # exit run loop
w.destroy                    # release native resources
w.destroyed?                 # true after destroy
```

### Native handles

```ruby
w.window_handle              # default UI window pointer
w.handle(:window)            # same as above
w.handle(:widget)            # browser widget
w.handle(:browser_controller)
```

### JS ↔ Ruby bindings

```ruby
w.bind('add') { |a, b| a + b }

w.bind('async_work') do |x|
  raise 'bad input' if x.negative?  # rejects the JS promise
  x * 2
end

w.unbind('add')
w.bindings    # => ['async_work']
```

The block receives the JSON-decoded arguments. Its return value is
JSON-encoded. Exceptions propagate to JavaScript as a rejected promise with
`{ name:, message: }`.

`bind` accepts either a block or any callable (`Proc`, `Method`, etc.).

### Dispatching from other threads / native callbacks

```ruby
w.dispatch do
  w.eval_script('console.log("from main thread")')
end
```

The block runs on the webview's main UI thread. Exceptions inside dispatch
blocks are swallowed (they have no caller to propagate to).

### Manual `webview_return`

```ruby
w.return_result(id, status, json_string)
```

Rarely needed — `bind` handles this for you.

### Version

```ruby
Webview.version
# => { major: 0, minor: 12, patch: 0,
#      version: "0.12.0", pre_release: "", build_metadata: "" }
```

### Constants

```ruby
Webview::HINT_NONE  Webview::HINT_MIN  Webview::HINT_MAX  Webview::HINT_FIXED
Webview::NATIVE_HANDLE_UI_WINDOW
Webview::NATIVE_HANDLE_UI_WIDGET
Webview::NATIVE_HANDLE_BROWSER_CONTROLLER
```

### Errors

All errors raised from this gem inherit from `Webview::Error`:

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

## Threading model

* `run` blocks the current thread.
* `bind` callbacks always run on the UI thread.
* `dispatch` is the safe way to interact with the webview from other threads
  or signal handlers.

## License

MIT — see [LICENSE](LICENSE).
