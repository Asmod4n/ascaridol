<div align="center">
  <img src="Hypha.svg" alt="Hypha" width="160" height="160">

<h1 align="center">Hypha</h1>

<p align="center"><strong>Desktop apps in mruby, with HTML/CSS/JS for the UI.</strong></p>

<p align="center">A single native binary, a webview, and Ruby for everything in between.</p>

---

## What is this

Hypha is a desktop application framework for [mruby](https://mruby.org). You
write your app's logic in Ruby, your UI in HTML/CSS/JS, and ship a single
native binary per platform. The Ruby and the page talk to each other over
JSON-bridged function calls. The native chrome (window, menus, OS integration)
is a thin shell built on [webview/webview](https://github.com/webview/webview).

Hypha is small. The runtime is a few thousand lines of C++ wrapping the
platform's existing browser engine — WebView2 on Windows, WKWebView on macOS,
WebKitGTK on Linux. There's no embedded Chromium and no cross-platform widget
toolkit; you don't build your own GTK port of every dialog. Just HTML.

```ruby
Hypha.run(title: "Hello", size: [800, 600]) do |h|
  h.bind(:greet) { |name| "Hello, #{name}!" }
  h.html = <<~HTML
    <h1>Hello, world</h1>
    <button onclick="greet('there').then(s => alert(s))">click me</button>
  HTML
end
```

That's a complete, working Hypha app.

## When you'd want this

You're a Ruby developer and you want to ship a desktop app without learning
JavaScript-as-application-language, Rust, or Go. You want the app to be small,
load fast, and not bundle 200MB of Chromium. You're OK writing your UI in
HTML/CSS — same as a web app, except local and with native window chrome.

Common shapes that fit Hypha well: configuration UIs for hardware projects,
local data viewers, dashboards over local services, dev tools, internal
utilities, hobby apps. Anything where the alternative would have been
"build a tiny web server and tell users to open localhost in their browser."

## When this is the wrong tool

If you need pixel-perfect native widgets, deep accessibility integration,
serious offline-first functionality at scale, or you're shipping to people
who'll evaluate you against Electron's polish, look elsewhere. Tauri is good.
Wails is good. Even Electron, despite the size, is mature.

Hypha is for the case where you'd otherwise be hand-rolling a webview wrapper
and you'd rather use a battle-tested one.

## Status

**v0.1.** Genuinely usable, not yet polished. The threading model is sound,
the platform code is real, the API has been redesigned twice and is now
stable enough to commit to. Expect rough edges. Bug reports welcome.

Tested on:
- Windows 11 (WebView2 + Edge)
- macOS 14+ (WKWebView)
- Linux (CachyOS/Arch)

## Install

Hypha ships as source. You build the `hypha` binary once, then any Hypha
app is a Ruby script that you compile against that binary.

```sh
git clone https://github.com/Asmod4n/hypha-mrb.git
cd hypha
rake
```

This produces `mruby/build/host/bin/hypha` (or `hypha.exe` on Windows). Drop
it on your `PATH`.

### Platform requirements

**Windows:** WebView2 SDK via NuGet (downloaded automatically as part of the
build). Visual Studio 2022 or Build Tools, x64 Native Tools Command Prompt.

**macOS:** Xcode command-line tools. WKWebView is part of macOS.

**Linux:** `pkg-config` and one of:
  - GTK 4 + WebKitGTK 6.0 (Debian 12+, Ubuntu 24.04+)
  - GTK 3 + WebKitGTK 4.1 (Debian 11, Ubuntu 22.04+)
  - GTK 3 + WebKitGTK 4.0 (older)

## The API

### `Hypha.run(**kwargs) { |h| ... }`

The single entry point. Creates the webview, applies kwargs, yields the
Hypha module to your block for further setup, then runs until the window
closes. Blocks the calling thread for the lifetime of the app.

```ruby
Hypha.run(
  title: "MyApp",          # window title
  size:  [900, 720],       # [width, height] or [width, height, hint]
  debug: false,            # devtools / right-click menu
  html:  "<h1>...</h1>",   # initial HTML (mutually exclusive with url:)
  url:   "https://...",    # initial URL
  init:  "console.log(1)"  # JS to run on every page load
) do |h|
  # do anything that requires the webview to be live:
  h.bind(:foo) { ... }
  h.add_native_event($stdin) { |io, evt| ... }
  h.html = render_initial_page
end
```

`Hypha.run` can only be called once per process. `size: [w, h, :fixed]`
locks the window to that size; other valid hints are `:none`, `:min`, `:max`.

### Setting content

```ruby
Hypha.html  = "<h1>...</h1>"   # set or replace page HTML
Hypha.url   = "https://..."     # navigate to a URL
Hypha.title = "New title"
Hypha.size  = [800, 600]        # also accepts [w, h, :hint]
Hypha.init "console.log('runs on every page load')"
Hypha.eval "document.body.style.background = 'red'"
```

All of these work from the setup block, from inside bind callbacks, and
from worker threads (where they dispatch onto main automatically).

### `Hypha.bind(name, &blk)`

Register a Ruby block that JavaScript can call. The JS side gets a Promise.

```ruby
Hypha.bind(:fetch_user) do |user_id|
  user = lookup_user(user_id)
  { name: user.name, email: user.email }   # auto-JSON-serialized
end
```

```javascript
fetch_user(42).then(user => {
  console.log(user.name);
});
```

Bind callbacks run on the main thread. Multiple bindings can be registered;
re-binding the same name replaces the previous block. Exceptions raised in
Ruby become rejected promises with a real `Error` object on the JS side
(name, message, and Ruby backtrace preserved).

`Hypha.bind` is main-thread-only — register all your bindings in the setup
block. Workers should send work to existing bindings via `Hypha.dispatch`,
not register new ones.

### `Hypha.add_native_event(io, &blk)`

Watch a file descriptor on the main run loop. The block fires when the fd
becomes ready.

```ruby
Hypha.add_native_event($stdin) do |io, events|
  line = io.gets
  Hypha.eval("console.log(#{line.to_json})")
  true   # return falsy to stop watching
end
```

Cross-platform: GTK uses `g_unix_fd_add`, macOS uses `CFFileDescriptor`,
Windows uses `WSAAsyncSelect` (so on Windows, the fd must be a winsock
socket, not a regular Win32 handle). Main-thread-only.

### `Hypha.dispatch(*args, &blk)`

The cross-thread escape hatch. mruby itself has no threads, but if you've
got a C extension that creates threads, those threads can push work back to
main via `Hypha.dispatch`:

```ruby
worker_thread = SomeThreadingGem.spawn do
  data = expensive_computation
  Hypha.dispatch(data) { |d| Hypha.html = render(d) }
end
```

The dispatched proc must be self-contained. References to outer-scope
variables won't work — the proc is serialized via mruby-cbor (with Procs
encoded as their irep dumps via `Proc#to_irep`) and reconstructed in main's
mrb_state. Pass data via positional arguments, not closures:

```ruby
# WRONG — captures `result` from the worker's scope
result = compute()
Hypha.dispatch { Hypha.html = render(result) }   # NoMethodError on main

# RIGHT — data flows in via args
result = compute()
Hypha.dispatch(result) { |r| Hypha.html = render(r) }
```

## The `rb-*` router

For form-driven UIs, Hypha provides an htmx-style attribute router. Drop the
generated `<script>` into your `<head>`:

```ruby
Hypha.run do |h|
  h.bind(:route) do |method, path, params|
    case "#{method} #{path}"
    when "GET /users" then render_user_list
    when "POST /users" then create_user(params); render_user_list
    # ...
    end
  end

  h.html = <<~HTML
    <head>#{Hypha.html_router(:route)}</head>
    <body>
      <button rb-get="/users" rb-target="#users">load</button>
      <div id="users"></div>
    </body>
  HTML
end
```

Supported attributes:
- `rb-get`, `rb-post`, `rb-put`, `rb-patch`, `rb-delete` — verb + path, fires `route`
- `rb-target` — CSS selector for where the response HTML goes
- `rb-swap` — `innerHTML` (default) or `outerHTML`
- `rb-trigger` — `"input changed delay:200ms, click"` style trigger spec
- `rb-vals` — JSON object merged into params
- `rb-indicator` — CSS selector for an element to mark `.busy` during requests

Forms harvest their named fields automatically. Lone form-controls (input,
select, textarea) contribute their name/value. `rb-vals` overrides both.

## Threading model

mruby is single-threaded. Hypha's design assumes that, and provides one
escape hatch (`Hypha.dispatch`) for users who bring their own threads via
C extensions.

The architectural rule: **only the main thread ever touches the `mrb_state` of Hypha.**
Hypha methods called from worker threads either dispatch a lambda onto main
(for value-only operations: `title=`, `html=`, `eval`, etc.) or raise
(for operations that need the main `mrb_state`: `bind`, `add_native_event`).

Procs cross thread boundaries via cbor: the proc's irep is dumped, sent as
bytes, and reconstructed on main. The proc must be self-contained — it can
take arguments but can't reference anything from the worker's scope. Captured
variables raise Errors at first invocation on main.

## Distribution and signing

Hypha ships unsigned by default. The first launch on Windows triggers
SmartScreen ("Windows protected your PC" → "More info" → "Run anyway"); on
macOS, Gatekeeper prompts ("unidentified developer" → right-click → Open).
After that one click, the binary runs normally.

For a smoother experience, you have a few options:

**Windows: SignPath Foundation.** [SignPath](https://signpath.org) provides
free OV-equivalent code signing for qualifying open-source projects, endorsed
by Microsoft. Apply, integrate their GitHub Action into your release workflow,
get signed releases automatically. SmartScreen reputation accumulates over
time. Most established OSS Windows tools sign through SignPath.

**Windows: Microsoft Trusted Signing.** $9.99/month, no certificate to manage,
integrates with CI. Cheapest legitimate option if SignPath doesn't fit.

**macOS: Apple Developer Program.** $99/year for notarization. No free
equivalent for OSS exists. Alternative: distribute via Homebrew tap (Homebrew
strips quarantine, no Gatekeeper warnings), or accept the right-click→Open
dance and document it.

**Linux:** No signing infrastructure. Just ship the binary.

For the Hypha project itself, signing is out of scope for v0.1. The README's
"first launch is annoying, every launch after is fine" UX is acceptable for
the technical audience Hypha is aimed at.

## Building apps with Hypha

Hypha apps are Ruby scripts compiled into the `hypha` binary at build time.
The Rakefile handles the embed-and-build cycle for you:

```sh
rake compile                              # builds with example/hello.rb
rake compile[example/dashboard.rb]        # builds with your script
rake 'compile[path/to/myapp.rb]'          # quote the brackets in zsh / fish
```

Or set the script via env var:

```sh
HYPHA_SCRIPT=example/dashboard.rb rake compile
```

The result is `mruby/build/host/bin/hypha` (or `hypha.exe` on Windows) with
your script embedded. Distribute that binary as your app.

If you want to embed a script without rebuilding the binary (e.g. while
iterating), `rake embed[your_script.rb]` regenerates `tools/hypha/main.c`
from the script. The next `rake compile` picks it up.

A more polished `hypha bundle` workflow (where users build their app from
the command line without touching the gem source) is planned for v0.2. For
v0.1, the build is manual but transparent.

## Project structure

If you're contributing or extending Hypha:

```
mruby-webview/
├── src/
│   ├── webview_internal.h     # globals declared, helpers inlined
│   └── hypha_methods.cpp      # Hypha.* methods that don't need platform code
│                              # (title=, html=, eval, dispatch, etc.)
├── mrblib/
│   └── hypha.rb               # CBOR Proc tag, html_router, bind wrapper
├── tools/
│   └── hypha/
│       └── hypha.cpp          # main(), Hypha.run, Hypha.bind, platform code
└── mrbgem.rake                # build configuration
```

The split between `src/` and `tools/hypha/` is meaningful: `src/` lives in
`libmruby.lib` and is linkable by other binaries (which see Hypha methods
that all raise "Hypha is not running"); `tools/hypha/` only links into the
`hypha` binary and provides the actual runtime.

## License

Apache-2.0. See LICENSE.

## Acknowledgments

Built on [webview/webview](https://github.com/webview/webview), which does
the actual work of wrapping per-platform browser engines. Hypha is a thin
mruby integration on top.
