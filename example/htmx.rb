# htmx desktop app via mruby-webview
#
# Run with:  ./bin/mruby example/htmx.rb
#
# The "mruby-router" extension works like htmx's own ws/sse extensions:
# it owns its own custom attributes (rb-post, rb-get, rb-delete, rb-target,
# rb-swap, rb-reset) and wires up DOM event listeners on htmx:load.
# htmx's request pipeline (XHR / fetch / beforeRequest) is never touched.

$count = 0
$todos = ["Buy milk", "Ship it"]

def render_count
  "<span id='count'>#{$count}</span>"
end

def render_todos
  items = $todos.each_with_index.map do |t, i|
    "<li class=\"todo-item\">" \
    "<span>#{t}</span>" \
    "<button class=\"del\"" \
    " rb-delete=\"/todos\"" \
    " rb-vals='{\"idx\":\"#{i}\"}'" \
    " rb-target=\"#todo-list\"" \
    " rb-swap=\"outerHTML\">x</button>" \
    "</li>"
  end.join
  "<ul id='todo-list'>#{items}</ul>"
end

# htmx extension: mruby-router
#
# Recognised attributes (mirror htmx naming, rb- prefix):
#   rb-get / rb-post / rb-put / rb-patch / rb-delete  — verb + path
#   rb-target   — CSS selector for the swap target (default: self)
#   rb-swap     — "innerHTML" (default) | "outerHTML"
#   rb-vals     — JSON object merged into params (same as hx-vals)
#   rb-reset    — if present on a <form>, reset it after a successful call
#
# The extension listens for htmx:load (fires whenever htmx processes new
# nodes, including the initial body) and attaches one event listener per
# element. That listener calls window.htmx_route — the Ruby binding — and
# performs the swap + htmx.process entirely in JS, with no dependency on
# htmx internals beyond htmx:load and htmx.process (both stable public API).
MRUBY_ROUTER_EXT = <<~'JS'
  <script>
    (function () {
      var SEL = '[rb-get],[rb-post],[rb-put],[rb-patch],[rb-delete]';
      var VERBS = ['get', 'post', 'put', 'patch', 'delete'];

      function dispatch(elt, e) {
        e.preventDefault();

        var method, path;
        for (var i = 0; i < VERBS.length; i++) {
          if (elt.hasAttribute('rb-' + VERBS[i])) {
            method = VERBS[i].toUpperCase();
            path   = elt.getAttribute('rb-' + VERBS[i]);
            break;
          }
        }

        var params = {};
        var raw = elt.getAttribute('rb-vals');
        if (raw) try { Object.assign(params, JSON.parse(raw)); } catch (_) {}
        if (elt.tagName === 'FORM') {
          new FormData(elt).forEach(function (v, k) { params[k] = v; });
        }

        var targetSel = elt.getAttribute('rb-target');
        var target    = targetSel ? document.querySelector(targetSel) : elt;
        var swap      = elt.getAttribute('rb-swap') || 'innerHTML';

        window.htmx_route(method, path, params).then(function (html) {
          if (swap === 'outerHTML') target.outerHTML = html;
          else                      target.innerHTML = html;
          if (elt.hasAttribute('rb-reset') && elt.tagName === 'FORM') elt.reset();
        }).catch(function (err) {
          console.error('[mruby-router]', err && err.message || err);
        });
      }

      document.addEventListener('click', function (e) {
        var elt = e.target.closest(SEL);
        if (!elt || elt.tagName === 'FORM') return;
        dispatch(elt, e);
      });

      document.addEventListener('submit', function (e) {
        if (!e.target.matches || !e.target.matches(SEL)) return;
        dispatch(e.target, e);
      });
    })();
  </script>
JS

def render_page
  <<~HTML
    <!doctype html>
    <html>
    <head>
      <meta charset="utf-8">
      <title>htmx desktop</title>
      <script src="https://cdn.jsdelivr.net/npm/htmx.org@2.0.10/dist/htmx.min.js"
          integrity="sha384-H5SrcfygHmAuTDZphMHqBJLc3FhssKjG7w/CeCpFReSfwBWDTKpkzPP8c+cLsK+V"
          crossorigin="anonymous"></script>
      #{MRUBY_ROUTER_EXT}
      <style>
        *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: 'Courier New', monospace; background: #0f0f0f; color: #e8e8e8; padding: 2rem; }
        h1 { font-size: 1.1rem; color: #888; letter-spacing: .2em; margin-bottom: 2rem; }
        h2 { font-size: .8rem; color: #555; letter-spacing: .15em; margin-bottom: 1rem; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 2rem; max-width: 640px; }
        .card { background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 4px; padding: 1.5rem; }
        .counter-display { font-size: 4rem; font-weight: bold; color: #c8f542; line-height: 1; margin-bottom: 1.2rem; }
        .btn-row { display: flex; gap: .5rem; }
        button {
          cursor: pointer; border: 1px solid #333; border-radius: 3px;
          background: #222; color: #e8e8e8; padding: .4rem .9rem;
          font-family: inherit; font-size: .85rem; transition: background .15s;
        }
        button:hover { background: #2e2e2e; }
        button.primary { border-color: #c8f542; color: #c8f542; }
        button.primary:hover { background: #c8f54218; }
        #todo-list { list-style: none; margin-bottom: 1rem; }
        .todo-item { display: flex; justify-content: space-between; align-items: center;
          padding: .4rem 0; border-bottom: 1px solid #222; font-size: .9rem; }
        .todo-item:last-child { border-bottom: none; }
        button.del { background: none; border: none; color: #555; padding: 0 .2rem; }
        button.del:hover { color: crimson; background: none; }
        .add-row { display: flex; gap: .5rem; margin-top: .5rem; }
        input[type=text] {
          flex: 1; background: #111; border: 1px solid #2a2a2a; border-radius: 3px;
          color: #e8e8e8; font-family: inherit; font-size: .85rem; padding: .4rem .7rem; outline: none;
        }
        input[type=text]:focus { border-color: #444; }
      </style>
    </head>
    <body hx-ext="mruby-router">
      <h1>HTMX x MRUBY</h1>
      <div class="grid">

        <div class="card">
          <h2>COUNTER</h2>
          <div class="counter-display">#{render_count}</div>
          <div class="btn-row">
            <button class="primary"
                    rb-post="/counter/increment"
                    rb-target="#count"
                    rb-swap="outerHTML">+ INC</button>
            <button rb-post="/counter/reset"
                    rb-target="#count"
                    rb-swap="outerHTML">RESET</button>
          </div>
        </div>

        <div class="card">
          <h2>TODOS</h2>
          #{render_todos}
          <form rb-post="/todos"
                rb-target="#todo-list"
                rb-swap="outerHTML"
                rb-reset>
            <div class="add-row">
              <input type="text" name="item" placeholder="add item..." autocomplete="off">
              <button class="primary" type="submit">ADD</button>
            </div>
          </form>
        </div>

      </div>
    </body>
    </html>
  HTML
end

def route(method, path, params)
  case [method, path]
  when ["POST", "/counter/increment"] then $count += 1; render_count
  when ["POST", "/counter/reset"]     then $count = 0;  render_count
  when ["POST", "/todos"]
    item = params["item"].to_s
    $todos << item unless item.empty?
    render_todos
  when ["DELETE", "/todos"]
    $todos.delete_at(params["idx"].to_i)
    render_todos
  else
    "<p style='color:crimson'>404 - #{method} #{path}</p>"
  end
end

Webview.open(title: "htmx x mruby", size: [800, 600], debug: true) do |w|
  w.bind(:htmx_route) { |method, path, params| route(method, path, params) }
  w.html = render_page
end