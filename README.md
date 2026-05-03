# mruby-webview

mruby bindings for [webview/webview](https://github.com/webview/webview):
build a desktop GUI with HTML/CSS/JS, backed by the system's native browser
engine (WebKitGTK on Linux, WebKit on macOS, WebView2 on Windows).

## Install

In your `build_config.rb`:

```ruby
conf.gem github: 'asmod4n/mruby-webview'
```

System packages:

* **Linux** — `gtk4` + `webkitgtk-6.0` (or `gtk+-3.0` + `webkit2gtk-4.1`/`4.0`).
  Debian/Ubuntu: `libgtk-4-dev libwebkitgtk-6.0-dev pkg-config`.
* **macOS** — Xcode command-line tools.
* **Windows** — WebView2 runtime (preinstalled on Windows 10/11).

The webview C++ source ships as a git submodule under `vendor/webview` and
is compiled into the gem; no CMake needed.

## Hello world

```ruby
Webview.open(title: 'Demo', size: [640, 480, :fixed]) do |w|
  w.bind('greet') { |name| "Hello, #{name}!" }
  w.html = <<~HTML
    <button onclick="greet('world').then(r => document.body.innerText = r)">Hi</button>
  HTML
end
```

## API

```ruby
w = Webview.new(debug: false, title: nil, size: nil, url: nil, html: nil)

w.title = 'My App'
w.set_size(800, 600, :fixed)   # :none / :min / :max / :fixed
w.url  = 'https://example.com'
w.html = '<h1>hi</h1>'
w.init_script(js)              # runs on every navigation
w.eval_script(js)              # eval in the current page

w.bind('add') { |a, b| a + b } # block return value resolves the JS promise;
                               # raising rejects it with { name:, message: }
w.unbind('add')

w.dispatch { ... }             # run a block on the UI thread
w.run                          # blocks until terminate / window closed
w.terminate
w.destroy
```

Errors raised from this gem all inherit from `Webview::Error`
(`MissingDependencyError`, `InvalidArgumentError`, `DuplicateError`,
`DestroyedError`, …).

## License

MIT
