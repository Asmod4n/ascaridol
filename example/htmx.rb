LOGO = <<~SVG
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="28" height="28" aria-hidden="true">
    <defs>
      <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#232933"/>
        <stop offset="1" stop-color="#0f1217"/>
      </linearGradient>
    </defs>
    <rect width="28" height="28" aria-hidden="true" rx="228" fill="url(#tile)"/>
    <g fill="none" stroke="#f2ead8" stroke-width="88" stroke-linecap="round" stroke-linejoin="round">
      <path d="M318 300 L150 512 L318 724"/>
      <path d="M706 300 L874 512 L706 724"/>
    </g>
    <path d="M432 360 L592 360 L676 450 L348 450 Z" fill="#ff6a82"/>
    <path d="M348 450 L676 450 L512 690 Z" fill="#d01c42"/>
    <path d="M432 360 L470 450 L512 690 L554 450 L592 360 Z" fill="#ffffff" fill-opacity="0.14"/>
  </svg>
SVG

CSS = <<~CSS
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Courier New', monospace; background: #0f0f0f; color: #e8e8e8; padding: 2rem; }
  h1 { display: flex; align-items: center; gap: .8rem; font-size: 1.1rem; color: #888; letter-spacing: .2em; margin-bottom: 2rem; }
  h2 { font-size: .8rem; color: #555; letter-spacing: .15em; margin-bottom: 1rem; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 2rem; max-width: 640px; }
  .card { background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 4px; padding: 1.5rem; }
  .counter-display { font-size: 4rem; font-weight: bold; color: #ef3355; line-height: 1; margin-bottom: 1.2rem; }
  .btn-row { display: flex; gap: .5rem; }
  button {
    cursor: pointer; border: 1px solid #333; border-radius: 3px;
    background: #222; color: #e8e8e8; padding: .4rem .9rem;
    font-family: inherit; font-size: .85rem; transition: background .15s;
  }
  button:hover { background: #2e2e2e; }
  button.primary { border-color: #ef3355; color: #ff6a82; }
  button.primary:hover { background: #ef335518; }
  #todo-list { list-style: none; margin-bottom: 1rem; }
  .todo-item { display: flex; justify-content: space-between; align-items: center;
    padding: .4rem 0; border-bottom: 1px solid #222; font-size: .9rem; }
  .todo-item:last-child { border-bottom: none; }
  button.del { background: none; border: none; color: #555; padding: 0 .2rem; }
  button.del:hover { color: #ef3355; background: none; }
  .add-row { display: flex; gap: .5rem; margin-top: .5rem; }
  input[type=text] {
    flex: 1; background: #111; border: 1px solid #2a2a2a; border-radius: 3px;
    color: #e8e8e8; font-family: inherit; font-size: .85rem; padding: .4rem .7rem; outline: none;
  }
  input[type=text]:focus { border-color: #444; }
CSS

def render_count(count)
  "<span id='count'>#{count}</span>"
end

def render_todos(todos)
  items = todos.each_with_index.map do |t, i|
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

def render_page(state)
  <<~HTML
    <!doctype html>
    <html>
    <head>
      <meta charset="utf-8">
      <title>htmx x mruby</title>
      #{Ascaridol.htmx(:route)}
      <style>#{CSS}</style>
    </head>
    <body>
      <h1>#{LOGO}HTMX x MRUBY</h1>
      <div class="grid">
        <div class="card">
          <h2>COUNTER</h2>
          <div class="counter-display">#{render_count(state[:count])}</div>
          <div class="btn-row">
            <button class="primary" hx-post="/counter/increment" hx-target="#count" hx-swap="outerHTML">+ INC</button>
            <button hx-post="/counter/reset" hx-target="#count" hx-swap="outerHTML">RESET</button>
          </div>
        </div>
        <div class="card">
          <h2>TODOS</h2>
          #{render_todos(state[:todos])}
          <form hx-post="/todos" hx-target="#todo-list" hx-swap="outerHTML" hx-on:htmx:after:request="this.reset()">
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

def route(state, method, path, params)
  case [method, path]
  when ["POST", "/counter/increment"]
    count = state[:count] + 1
    [state.merge(count: count), render_count(count)]
  when ["POST", "/counter/reset"]
    [state.merge(count: 0), render_count(0)]
  when ["POST", "/todos"]
    item = params["item"].to_s
    todos = item.empty? ? state[:todos] : state[:todos] + [item]
    [state.merge(todos: todos), render_todos(todos)]
  when ["DELETE", "/todos"]
    idx = params["idx"].to_i
    todos = state[:todos].reject.with_index { |_, i| i == idx }
    [state.merge(todos: todos), render_todos(todos)]
  else
    [state, "<p style='color:#ef3355'>404 - #{method} #{path}</p>"]
  end
end

def main
  state = { count: 0, todos: ["Buy milk", "Ship it"] }
  Ascaridol.run(title: "htmx x mruby", size: [800, 600]) do |w|
    w.bind(:route) do |method, path, params|
      state, html = route(state, method, path, params)
      html
    end
    w.html = render_page(state)
  end
end
