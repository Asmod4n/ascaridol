<div align="center">
  <img src="Hypha.svg" alt="Hypha" width="160" height="160">

<h1>Hypha</h1>

<p><strong>Desktop apps in mruby, with HTML/CSS/JS for the UI.</strong></p>

<p>A single native binary, a webview, and Ruby for everything in between.</p>
</div>

## What is this

Hypha is a desktop application framework for [mruby](https://mruby.org).
You write your app's logic in Ruby, your UI in HTML/CSS/JS, and ship a
single native binary per platform. Ruby and the page talk over
JSON-bridged function calls. The native chrome is a thin shell built on
[webview/webview](https://github.com/webview/webview) — WebView2 on
Windows, WKWebView on macOS, WebKitGTK on Linux. No embedded Chromium,
no cross-platform widget toolkit.

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

Ship a small desktop app in Ruby without learning JS-as-application,
Rust, or Go. Configuration UIs for hardware, local data viewers,
dashboards, dev tools, internal utilities, hobby apps — anything where
the alternative would have been "run a tiny web server and tell users
to open localhost."

## Status

**v0.2.** Genuinely usable, not yet polished. Threading model is sound,
platform code is real, API is stable. Bug reports welcome.

Tested on Windows 11, macOS 14+, Linux (Arch, openSUSE Tumbleweed).

## Install

Add Hypha to your `build_config.rb` and point `hypha_main` at your app:

```ruby
conf.gem github: 'Asmod4n/hypha-mrb' do |hypha|
  hypha.hypha_main = 'app/main.rb'
end
```

Then `rake`. The result is `mruby/build/host/bin/hypha` (or `hypha.exe`
on Windows) with your script embedded. Distribute it as your app.

To try a bundled example without writing a build_config:

```sh
git clone https://github.com/Asmod4n/hypha-mrb.git
cd hypha-mrb
HYPHA_SCRIPT=example/dashboard.rb rake
```

Relative paths resolve against the gem directory. `HYPHA_SCRIPT`
overrides `hypha_main` for one-off builds.

### Ruby

mruby's build uses Rake, so you need a Ruby (any recent MRI) at build
time. It's not embedded in the final binary — just drives the build.

- **Linux:** `pacman -S ruby` / `zypper install ruby` /
  `apt install ruby` — system Ruby works fine.
- **macOS:** `brew install ruby` (system Ruby is being phased out).
- **Windows:** [RubyInstaller](https://rubyinstaller.org). Skip the
  MSYS2 development kit prompt — Hypha builds against MSVC, you don't
  need MinGW. Always run `rake` from the **x64 Native Tools Command
  Prompt for Visual Studio 2022** so `cl.exe` and the Windows SDK are
  on `PATH`.

### Platform requirements

**Windows:** Visual Studio 2022 or Build Tools, x64 Native Tools Command
Prompt. WebView2 SDK is fetched automatically.

**macOS:** Xcode command-line tools.

**Linux:** `pkg-config` and one of:
  - GTK 4 + WebKitGTK 6.0 (Debian 12+, Ubuntu 24.04+)
  - GTK 3 + WebKitGTK 4.1 (Debian 11, Ubuntu 22.04+)
  - GTK 3 + WebKitGTK 4.0 (older)

## The API

### `Hypha.run(**kwargs) { |h| ... }`

The single entry point. Creates the webview, applies kwargs, yields the
Hypha module to your block, then runs until the window closes.

```ruby
Hypha.run(
  title: "MyApp",
  size:  [900, 720],          # or [w, h, :fixed | :none | :min | :max]
  html:  "<h1>...</h1>",      # initial HTML (mutually exclusive with url:)
  url:   "https://...",       # initial URL
  init:  "console.log(1)"     # JS to run on every page load
) do |h|
  h.bind(:foo) { ... }
  h.html = render_initial_page
end
```

`Hypha.run` is not re-entrant — you can't call it from inside its own
setup block or from a bind callback — but you can call it again after
it returns. Each call creates a fresh window and runs until that
window closes.

### Setting content

```ruby
Hypha.html  = "<h1>...</h1>"
Hypha.url   = "https://..."
Hypha.title = "New title"
Hypha.size  = [800, 600]
Hypha.init  "console.log('runs on every page load')"
Hypha.eval  "document.body.style.background = 'red'"
```

All of these work from the setup block, from bind callbacks, and from
worker threads (where they dispatch onto main automatically).

### `Hypha.bind(name, &blk)`

Register a Ruby block JavaScript can call. JS gets a Promise.

```ruby
Hypha.bind(:fetch_user) do |user_id|
  user = lookup_user(user_id)
  { name: user.name, email: user.email }
end
```

```javascript
fetch_user(42).then(user => console.log(user.name));
```

Bind callbacks run on main. Re-binding the same name replaces the
previous block. Exceptions raised in Ruby become rejected promises with
a real `Error` object on the JS side (name, message, and Ruby backtrace
preserved).

Main-thread-only. Register all bindings in the setup block.

### `Hypha.bind_async(name, &blk)`

Like `bind`, but the answer is deferred. The block gets an `id` plus
any JS args and is expected to call `Hypha.resolve(id) { ... }` later —
possibly much later, possibly from another thread. Use this for I/O,
timers, user interaction, worker results.

```ruby
Hypha.bind_async(:wait_for_ping) do |id, *args|
  @pending = id
end
```

The block's return value is ignored — resolution happens through
`Hypha.resolve`, not return. If the block raises before stashing the
id anywhere, the promise auto-rejects so JS doesn't hang. Drop the id
without resolving and the promise leaks forever.

Sync and async names share a single JS-side namespace but live in
separate Ruby registries; `Hypha.unbind(name)` clears whichever.

See [`example/bind_async.rb`](example/bind_async.rb).

### `Hypha.resolve(id, &blk)`

Settle a pending `bind_async` call. Block return is JSON-encoded and
shipped to JS; if it raises, the promise rejects with the exception.
Thread-safe — call it from anywhere.

### `Hypha.poll_add(io, readiness = :r, &blk)`

Watch a file descriptor on the main run loop. Returns a
`Hypha::Watcher`.

```ruby
watcher = Hypha.poll_add($stdin) do |io, cond|
  line = io.gets
  Hypha.eval("console.log(#{JSON.dump(line)})")
  true   # falsy stops watching
end
```

Readiness: `:r` (readable), `:w` (writable), `:rw` (both). Block gets
`(io, cond)` where `cond` is the Symbol describing the wakeup
(`:r`, `:w`, `:rw`, or `:err`).

**Windows:** the fd must be a winsock `SOCKET`; `WSAAsyncSelect`
side-effects the socket into non-blocking and locks out further
`ioctlsocket(FIONBIO)` changes.

**Linux / macOS:** the fd's blocking flag is untouched. Call
`io._setnonblock(true)` first or a `recv` after a spurious wakeup will
stall the run loop.

Main-thread-only. See [`example/echo_server.rb`](example/echo_server.rb).

### `Hypha::Watcher`

| Method                        | What it does                              |
|-------------------------------|-------------------------------------------|
| `#io`                         | the IO/socket the watcher is attached to  |
| `#update(:r \| :w \| :rw)`    | change readiness in place                 |
| `#remove`                     | stop watching                             |

Use `#update` to toggle write-readiness: subscribe to `:r`, switch to
`:rw` when your outbox grows, back to `:r` when it drains. After
`#remove` (or after the block returns falsy), the watcher is dead;
`#update` and `#remove` raise `IOError`.

### `Hypha.dispatch(*args, &blk)`

The cross-thread escape hatch. mruby itself has no threads, but C
extensions can create them. Those threads push work back to main via
`Hypha.dispatch`:

```ruby
Hypha.dispatch(result) { |r| Hypha.html = render(r) }
```

The proc is serialized (its irep is dumped via `Proc#to_irep` and
shipped as CBOR bytes) and reconstructed inside main's `mrb_state`.
Same goes for `Hypha.resolve`'s block, and for any other Hypha entry
point that's documented as thread-safe.

**What survives serialization:**

- The proc's own bytecode and literals.
- Arguments passed alongside the proc, as long as each argument is
  itself CBOR-encodable. Strings, integers, floats, true/false/nil,
  symbols, arrays and hashes of the above, and other Procs all work.
  Custom classes work only if you've registered a CBOR tag for them.

**What does not survive:**

- Captured outer-scope locals. `result = 42; dispatch { result }` looks
  like it should work but raises on main — the local `result` doesn't
  exist there.
- Captured `self` and any instance variables that came with it.
- Anything heap-allocated on the worker's VM (file handles, sockets,
  C-extension wrappers): the references are meaningless on main.

**Rule of thumb:** write the proc as if it's being eval'd at the top
level on a fresh VM. Everything the proc needs comes in through its
arguments:

```ruby
# WRONG — closes over `data` from worker's scope
data = fetch_something
Hypha.dispatch { Hypha.html = render(data) }

# RIGHT — `data` flows in as an arg
data = fetch_something
Hypha.dispatch(data) { |d| Hypha.html = render(d) }
```

If the proc raises on main, the exception is printed via
`mrb_print_error` (stderr by default). The dispatching worker is
already gone; nothing propagates back.

### Smaller methods

| Method                       | What it does                                                  |
|------------------------------|---------------------------------------------------------------|
| `Hypha.ready { ... }`        | One-shot hook fired once after setup, before the run loop pumps. Raises on second set. |
| `Hypha.unbind(name)`         | Remove a sync or async binding. Main only.                    |
| `Hypha.bindings`             | Array of registered binding names (Symbols). Main only.       |
| `Hypha.terminate`            | Close the window and exit the run loop.                       |
| `Hypha.running?`             | True between `Hypha.run` starting and the run loop exiting.   |
| `Hypha.version`              | Hash with libwebview version info (`:version`, `:major`, `:minor`, `:patch`, `:pre_release`, `:build_metadata`). |
| `Hypha.platform`             | Platform identifier Symbol.                                   |
| `Hypha.handle(kind=:window)` | Native handle by kind — `:window`, `:widget`, or `:browser_controller`. Main only. |

## The `rb-*` router

htmx-style attribute router for form-driven UIs. Drop the generated
`<script>` into `<head>`:

```ruby
Hypha.run do |h|
  h.bind(:route) do |method, path, params|
    case "#{method} #{path}"
    when "GET /users"  then render_user_list
    when "POST /users" then create_user(params); render_user_list
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

Attributes: `rb-get`/`rb-post`/`rb-put`/`rb-patch`/`rb-delete` (verb +
path), `rb-target` (CSS selector), `rb-swap` (`innerHTML` /
`outerHTML`), `rb-trigger` (`"input changed delay:200ms, click"`),
`rb-vals` (JSON merged into params), `rb-indicator` (CSS selector
marked `.busy` during requests).

Forms harvest named fields automatically. Lone controls contribute
their name/value. `rb-vals` overrides both.

## Threading model

mruby is single-threaded. Only the main thread ever touches Hypha's
`mrb_state`.

Hypha methods called from worker threads either dispatch onto main
(value-only ops: `title=`, `html=`, `eval`) or raise (ops that need the
main `mrb_state`: `bind`, `poll_add`).

For the details of how procs and arguments cross the boundary, see
[`Hypha.dispatch`](#hyphadispatchargs-blk) above.

## Distribution and signing

Hypha ships unsigned by default. First launch on Windows triggers
SmartScreen, on macOS triggers Gatekeeper. After one click, the binary
runs normally.

For a smoother experience:

- **Windows:** [SignPath Foundation](https://signpath.org) offers free
  code signing for qualifying OSS. Microsoft Trusted Signing at
  $9.99/month is the cheapest paid option.
- **macOS:** Apple Developer Program ($99/yr) for notarization, or
  distribute via a Homebrew tap.
- **Linux:** no signing infrastructure; just ship the binary.

## Project structure

```
hypha-mrb/
├── src/                       # linked into libmruby
├── mrblib/hypha.rb            # Ruby-level helpers
├── tools/hypha/
│   ├── hypha.cc               # main(), platform code
│   ├── stub.rb                # default "no app embedded" script
│   └── main.c                 # generated by mrbc at build time
└── mrbgem.rake
```

`tools/hypha/main.c` is regenerated on every build from whatever script
`hypha_main` points at. It's tracked in git so a fresh clone can build;
don't commit local changes to it (`git checkout tools/hypha/main.c` to
discard).

## License

MIT. See LICENSE.

## Acknowledgments

Built on [webview/webview](https://github.com/webview/webview).