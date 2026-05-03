# htmx desktop app via mruby-webview
#
# Run with:  ./bin/mruby htmx.rb
#
# Instead of replacing XMLHttpRequest/fetch, we hook htmx's own event system:
#   htmx:beforeRequest → preventDefault → call Ruby → swap via htmx API
#
# Only the URL constructor is patched, because htmx calls
# new URL(path, document.location.href) for origin-validation before the
# event fires, and document.location.href is "about:blank" in set_html pages.

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
    " hx-delete=\"/todos\"" \
    " hx-vals='{\"idx\":\"#{i}\"}'" \
    " hx-target=\"#todo-list\"" \
    " hx-swap=\"outerHTML\">x</button>" \
    "</li>"
  end.join
  "<ul id='todo-list'>#{items}</ul>"
end

def render_page
  <<~HTML
    <!doctype html>
    <html>
    <head>
      <meta charset="utf-8">
      <title>htmx desktop</title>
      <base href="http://localhost/">
      #{URL_PATCH}
      <script src="https://cdn.jsdelivr.net/npm/htmx.org@2.0.10/dist/htmx.min.js" 
          integrity="sha384-H5SrcfygHmAuTDZphMHqBJLc3FhssKjG7w/CeCpFReSfwBWDTKpkzPP8c+cLsK+V" crossorigin="anonymous"></script>
      #{HTMX_HOOK}
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
    <body>
      <h1>HTMX x MRUBY</h1>
      <div class="grid">

        <div class="card">
          <h2>COUNTER</h2>
          <div class="counter-display">#{render_count}</div>
          <div class="btn-row">
            <button class="primary"
                    hx-post="/counter/increment"
                    hx-target="#count"
                    hx-swap="outerHTML">+ INC</button>
            <button hx-post="/counter/reset"
                    hx-target="#count"
                    hx-swap="outerHTML">RESET</button>
          </div>
        </div>

        <div class="card">
          <h2>TODOS</h2>
          #{render_todos}
          <form hx-post="/todos"
                hx-target="#todo-list"
                hx-swap="outerHTML"
                hx-on::after-request="this.reset()">
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

# Fixes new URL(path, "about:blank") throwing in WebKit.
# Everything else in the browser stack stays untouched.
URL_PATCH = <<~'JS'
  <script>
    (function() {
      var Native = window.URL;
      function PatchedURL(url, base) {
        if (!base || base === 'about:blank' || base === 'null') base = 'http://localhost/';
        return new Native(url, base);
      }
      PatchedURL.prototype       = Native.prototype;
      PatchedURL.createObjectURL = Native.createObjectURL.bind(Native);
      PatchedURL.revokeObjectURL = Native.revokeObjectURL.bind(Native);
      if (Native.canParse) PatchedURL.canParse = Native.canParse.bind(Native);
      window.URL = PatchedURL;
    })();
  </script>
JS

# Runs after htmx loads. Uses htmx's own event system instead of faking XHR:
#   1. selfRequestsOnly off  — origin is "null" on about:blank pages
#   2. htmx:beforeRequest    — cancel the real request
#   3. grab method/path/params from evt.detail (htmx already parsed them)
#   4. call Ruby, then swap via htmx.find + outerHTML/innerHTML
#   5. htmx.process() re-registers htmx on any new elements
#   6. htmx.trigger afterRequest so hx-on::after-request handlers fire
HTMX_HOOK = <<~'JS'
  <script>
    htmx.config.selfRequestsOnly = false;

    document.addEventListener('htmx:beforeRequest', function(evt) {
      evt.preventDefault();

      var elt    = evt.detail.elt;
      var target = evt.detail.target;
      var method = (evt.detail.requestConfig.verb || 'get').toUpperCase();
      var path   = evt.detail.pathInfo && evt.detail.pathInfo.requestPath;
      var params = evt.detail.requestConfig.parameters || {};

      if (!path) return;

      // Strip the dummy base, keep only the path
      try { path = new URL(path).pathname; } catch(e) {}

      window.htmx_route(method, path, params).then(function(html) {
        var swap = elt.getAttribute('hx-swap') ||
                   htmx.config.defaultSwapStyle || 'innerHTML';

        if (swap === 'outerHTML') {
          target.outerHTML = html;
        } else {
          target.innerHTML = html;
        }

        // Re-register htmx on any newly inserted elements
        htmx.process(document.body);

        // Fire afterRequest so hx-on::after-request="this.reset()" etc. work
        htmx.trigger(elt, 'htmx:afterRequest', { successful: true, failed: false });

      }).catch(function(err) {
        console.error('htmx_route error:', err, err && err.message);
        htmx.trigger(elt, 'htmx:afterRequest', { successful: false, failed: true });
      });
    });
  </script>
JS

Webview.open(title: "htmx x mruby", size: [800, 600], debug: true) do |w|
  w.bind(:htmx_route) { |method, path, params| route(method, path, params) }
  w.html = render_page
end