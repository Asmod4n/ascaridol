# Form validation patterns in mruby-webview.
#
# Run with:  ./bin/mruby examples/forms.rb
#
# Demonstrates:
#   - Inline field validation (rb-post on input blur)
#   - Multi-target swaps (errors -> field, summary -> footer)
#   - Form submit with full-form replace on success
#   - Server-driven password strength meter
#
# The mruby-router extension handles `rb-trigger="blur"` so individual
# fields can validate themselves without wiring custom JS.

$users = ["alice", "bob"] # already-taken handles


CSS = <<~'CSS'
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Courier New', monospace; background: #0f0f0f; color: #e8e8e8; padding: 2rem; }
  h1 { font-size: 1.1rem; color: #888; letter-spacing: .2em; margin-bottom: 2rem; }
  h2 { font-size: .8rem; color: #555; letter-spacing: .15em; margin-bottom: 1rem; }
  .card { background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 4px;
          padding: 1.5rem; max-width: 480px; }
  .field { margin-bottom: 1rem; }
  label { display: block; font-size: .75rem; color: #888; letter-spacing: .1em;
          margin-bottom: .35rem; }
  input[type=text], input[type=password], input[type=email] {
    width: 100%; background: #111; border: 1px solid #2a2a2a; border-radius: 3px;
    color: #e8e8e8; font-family: inherit; font-size: .9rem; padding: .55rem .7rem;
    outline: none;
  }
  input:focus { border-color: #444; }
  input.invalid { border-color: crimson; }
  input.valid   { border-color: #c8f542; }
  .err { font-size: .75rem; color: crimson; margin-top: .3rem; min-height: 1rem; }
  .ok  { font-size: .75rem; color: #c8f542; margin-top: .3rem; min-height: 1rem; }
  button {
    cursor: pointer; border: 1px solid #c8f542; border-radius: 3px;
    background: transparent; color: #c8f542; padding: .55rem 1.1rem;
    font-family: inherit; font-size: .85rem;
  }
  button:hover { background: #c8f54218; }
  button:disabled { opacity: .35; cursor: not-allowed; }
  .meter { display: flex; gap: 4px; margin-top: .4rem; }
  .meter span { flex: 1; height: 4px; background: #2a2a2a; border-radius: 2px; }
  .meter span.on-1 { background: crimson; }
  .meter span.on-2 { background: #d97706; }
  .meter span.on-3 { background: #c8f542; }
  .summary { margin-top: 1.2rem; padding-top: 1rem; border-top: 1px solid #2a2a2a;
             font-size: .8rem; color: #888; }
  .summary.success { color: #c8f542; }
CSS

def render_username_field(name = "", state = nil)
  cls = case state when :ok then "valid" when :err then "invalid" else "" end
  msg = case state
        when :err then "<div class='err' id='username-msg'>handle is taken</div>"
        when :ok  then "<div class='ok'  id='username-msg'>handle is free</div>"
        else           "<div class='err' id='username-msg'></div>"
        end
  <<~HTML
    <div class="field" id="username-field">
      <label>HANDLE</label>
      <input type="text" name="username" value="#{name}" class="#{cls}"
             autocomplete="off"
             rb-post="/validate/username"
             rb-trigger="blur"
             rb-target="#username-field"
             rb-swap="outerHTML">
      #{msg}
    </div>
  HTML
end

# Render only the strength meter — keeping the <input> stable across
# keystrokes so it never loses focus.
def render_password_meter(pw = "")
  score = password_score(pw)
  meter = (1..3).map { |i| "<span class='#{score >= i ? "on-#{score}" : ""}'></span>" }.join
  label = ["", "weak", "ok", "strong"][score]
  <<~HTML
    <div id="password-meter">
      <div class="meter">#{meter}</div>
      <div class="ok">#{label}</div>
    </div>
  HTML
end

def render_password_field(pw = "")
  <<~HTML
    <div class="field" id="password-field">
      <label>PASSWORD</label>
      <input type="password" name="password" value="#{pw}"
             rb-post="/validate/password"
             rb-trigger="input"
             rb-target="#password-meter"
             rb-swap="outerHTML">
      #{render_password_meter(pw)}
    </div>
  HTML
end

def password_score(pw)
  s = 0
  s += 1 if pw.length >= 8
  has_upper = pw.each_char.any? { |c| c >= "A" && c <= "Z" }
  has_lower = pw.each_char.any? { |c| c >= "a" && c <= "z" }
  has_other = pw.each_char.any? { |c| (c >= "0" && c <= "9") || !((c >= "A" && c <= "Z") || (c >= "a" && c <= "z")) }
  s += 1 if has_upper && has_lower
  s += 1 if has_other
  s
end

def render_form(state = {})
  <<~HTML
    <form rb-post="/signup" rb-target="#card" rb-swap="outerHTML">
      #{render_username_field(state[:username] || "")}
      #{render_password_field(state[:password] || "")}
      <button type="submit">CREATE ACCOUNT</button>
      <div class="summary" id="summary"></div>
    </form>
  HTML
end

def render_card(inner)
  "<div class='card' id='card'><h2>SIGN UP</h2>#{inner}</div>"
end

def render_page
  <<~HTML
    <!doctype html><html><head><meta charset="utf-8"><title>forms</title>
    #{Hypha.html_router(:route)}<style>#{CSS}</style></head>
    <body><h1>FORMS x VALIDATION</h1>
    #{render_card(render_form)}
    </body></html>
  HTML
end

def route(method, path, params)
  case [method, path]
  when ["POST", "/validate/username"]
    name = params["username"].to_s.strip
    return render_username_field(name, nil)               if name.empty?
    return render_username_field(name, :err)              if $users.include?(name.downcase)
    render_username_field(name, :ok)
  when ["POST", "/validate/password"]
    render_password_meter(params["password"].to_s)
  when ["POST", "/signup"]
    name = params["username"].to_s.strip
    pw   = params["password"].to_s
    if name.empty? || $users.include?(name.downcase) || password_score(pw) < 2
      render_card(render_form(username: name, password: pw) +
                  "<div class='summary' style='color:crimson'>fix errors above</div>")
    else
      $users << name.downcase
      render_card("<div class='summary success'>welcome, #{name} ✓</div>")
    end
  else
    "<p style='color:crimson'>404 #{method} #{path}</p>"
  end
end

Hypha.run(title: "forms x mruby", size: [600, 600]) do |w|
  w.bind(:route) { |m, p, params| route(m, p, params) }
  w.html = render_page
end
