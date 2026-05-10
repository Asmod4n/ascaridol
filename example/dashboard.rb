# A live introspection dashboard for mruby + mruby-webview.
#
# Run with:  ./bin/mruby examples/dashboard/app.rb
#
# Demonstrates:
#   - rb-indicator: busy bar shown while a Ruby binding runs
#   - Long-running work (a benchmark loop) without locking the UI
#   - Structured output: status pill + headers-style key/value panel
#   - Surfacing exceptions raised from Ruby as visible errors
#   - Running an arbitrary Ruby snippet from the UI and showing the result
#
# Pure stdlib — no extra mgems required. This is the example to read first
# if you want to see how to invoke heavier Ruby work from htmx and present
# the results.

# ---- collectors -----------------------------------------------------------

def webview_info
  v = Hypha.version rescue nil
  return { "available" => "false" } unless v
  {
    "version"         => v[:version],
    "major.minor.patch" => "#{v[:major]}.#{v[:minor]}.#{v[:patch]}",
    "pre_release"     => v[:pre_release].to_s,
    "build_metadata"  => v[:build_metadata].to_s,
  }
end

def mruby_info
  {
    "RUBY_VERSION"      => (Object.const_defined?(:RUBY_VERSION)      ? RUBY_VERSION      : "?"),
    "RUBY_ENGINE"       => (Object.const_defined?(:RUBY_ENGINE)       ? RUBY_ENGINE       : "?"),
    "MRUBY_VERSION"     => (Object.const_defined?(:MRUBY_VERSION)     ? MRUBY_VERSION     : "?"),
    "MRUBY_RELEASE_NO"  => (Object.const_defined?(:MRUBY_RELEASE_NO)  ? MRUBY_RELEASE_NO.to_s  : "?"),
    "RUBY_PLATFORM"     => (Object.const_defined?(:RUBY_PLATFORM)     ? RUBY_PLATFORM     : "?"),
  }
end

def env_info
  # ENV is part of mruby-env, which isn't bundled with most builds.
  if Object.const_defined?(:ENV)
    keys = %w[HOME USER LANG SHELL XDG_DATA_HOME LOCALAPPDATA]
    out = {}
    keys.each do |k|
      v = ENV[k] rescue nil
      out[k] = v[0, 80] if v
    end
    out["(empty)"] = "no ENV vars matched" if out.empty?
    out
  else
    { "ENV" => "(mruby-env mgem not loaded)" }
  end
end

# A toy benchmark: how many integer ops per second can mruby do? Runs for ~N ms
# of wall time and returns ops, ms, ops/sec.
def benchmark(target_ms)
  ops = 0
  t0 = Time.now rescue nil
  loop do
    1000.times { ops += 1 }
    t1 = Time.now rescue nil
    break if t0.nil? || t1.nil? || (t1 - t0) * 1000 >= target_ms
  end
  t1 = Time.now rescue nil
  ms = (t0 && t1) ? ((t1 - t0) * 1000).round : 0
  rate = ms > 0 ? (ops * 1000 / ms) : 0
  { ops: ops, ms: ms, rate: rate }
end


# ---- htmx router ----------------------------------------------------------

CSS = <<~'CSS'
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Courier New', monospace; background: #0f0f0f; color: #e8e8e8; padding: 1.5rem; }
  h1 { font-size: 1rem; color: #888; letter-spacing: .2em; margin-bottom: 1.5rem; }
  h2 { font-size: .7rem; color: #555; letter-spacing: .15em; margin-bottom: .8rem; }

  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1.2rem; max-width: 980px; }
  .panel { background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 4px; padding: 1.2rem 1.4rem; }
  .panel.full { grid-column: 1 / -1; }

  .kv { display: grid; grid-template-columns: max-content 1fr; gap: .35rem 1.2rem; font-size: .8rem; }
  .kv .k { color: #888; }
  .kv .v { color: #ddd; word-break: break-all; }

  .row { display: flex; align-items: center; gap: .8rem; flex-wrap: wrap; }
  .pill { display: inline-block; padding: .15rem .6rem; border-radius: 99px; font-size: .72rem;
          border: 1px solid currentColor; letter-spacing: .12em; }
  .pill.ok   { color: #c8f542; }
  .pill.warn { color: #d97706; }
  .pill.err  { color: crimson; }

  button {
    cursor: pointer; border: 1px solid #c8f542; background: transparent; color: #c8f542;
    padding: .4rem 1rem; border-radius: 3px; font-family: inherit; font-size: .8rem;
    letter-spacing: .12em;
  }
  button:hover { background: #c8f54218; }
  button:disabled { opacity: .35; cursor: wait; }

  input[type=number], textarea {
    background: #111; border: 1px solid #2a2a2a; color: #e8e8e8;
    font-family: inherit; font-size: .85rem; padding: .5rem .7rem; border-radius: 3px;
    outline: none; width: 100%;
  }
  textarea { resize: vertical; min-height: 70px; line-height: 1.5; }
  input:focus, textarea:focus { border-color: #444; }

  .indicator { height: 2px; background: transparent; margin: .8rem 0 0 0; overflow: hidden; }
  .indicator.busy {
    background: linear-gradient(90deg, transparent, #c8f542, transparent);
    background-size: 40% 100%; background-repeat: no-repeat;
    animation: slide 1.1s linear infinite;
  }
  @keyframes slide { 0% { background-position: -40% 0; } 100% { background-position: 140% 0; } }

  pre {
    font: inherit; font-size: .8rem; color: #ddd; white-space: pre-wrap; word-break: break-word;
    background: #111; border: 1px solid #2a2a2a; border-radius: 3px; padding: .8rem;
    line-height: 1.5;
  }
  .error { color: crimson; padding: .8rem 1rem; border: 1px solid crimson; border-radius: 4px;
           font-size: .85rem; }
  .timing { color: #666; font-size: .75rem; letter-spacing: .1em; }
  .stat { font-size: 1.6rem; color: #c8f542; font-variant-numeric: tabular-nums; line-height: 1; }
  .stat-label { font-size: .65rem; color: #555; letter-spacing: .15em; margin-top: .3rem; }
  .stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 1rem; margin-bottom: 1rem; }
  .form-row { display: grid; grid-template-columns: auto 1fr auto; gap: .5rem; align-items: center; }
CSS

# ---- rendering ------------------------------------------------------------

def html_escape(s)
  s.to_s.gsub("&", "&amp;").gsub("<", "&lt;").gsub(">", "&gt;").gsub('"', "&quot;")
end

def render_kv(hash)
  hash.map { |k, v| "<span class='k'>#{html_escape(k)}</span><span class='v'>#{html_escape(v)}</span>" }.join
end

def render_webview_panel
  v = webview_info
  status = v["available"] == "false" ? "<span class='pill err'>OFFLINE</span>"
                                     : "<span class='pill ok'>OK</span>"
  <<~HTML
    <div class="panel">
      <h2>WEBVIEW</h2>
      <div class="row" style="margin-bottom:.8rem">#{status}<span class="timing">Webview.version</span></div>
      <div class="kv">#{render_kv(v)}</div>
    </div>
  HTML
end

def render_mruby_panel
  <<~HTML
    <div class="panel">
      <h2>MRUBY</h2>
      <div class="kv">#{render_kv(mruby_info)}</div>
    </div>
  HTML
end

def render_env_panel
  <<~HTML
    <div class="panel full">
      <h2>ENVIRONMENT</h2>
      <div class="kv">#{render_kv(env_info)}</div>
    </div>
  HTML
end

def render_bindings_panel(w_bindings)
  rows = w_bindings.empty? ? "<span class='timing'>(no bindings registered yet)</span>"
                            : w_bindings.map { |n| "<span class='pill ok'>#{html_escape(n)}</span>" }.join(" ")
  <<~HTML
    <div class="panel full">
      <h2>JS BINDINGS · #{w_bindings.size}</h2>
      <div class="row">#{rows}</div>
    </div>
  HTML
end

def render_bench_form(default_ms = 200)
  <<~HTML
    <form id="bench-form" rb-post="/bench" rb-target="#bench-result" rb-swap="outerHTML"
          rb-indicator="#bench-indicator">
      <div class="form-row">
        <label class="timing">RUN FOR</label>
        <input type="number" name="ms" value="#{default_ms}" min="50" max="5000" step="50">
        <button type="submit">START</button>
      </div>
      <div id="bench-indicator" class="indicator"></div>
    </form>
  HTML
end

def render_bench_result(r = nil)
  if r.nil?
    <<~HTML
      <div id="bench-result" class="stats">
        <div><div class="stat">—</div><div class="stat-label">OPS</div></div>
        <div><div class="stat">—</div><div class="stat-label">ELAPSED</div></div>
        <div><div class="stat">—</div><div class="stat-label">OPS / SEC</div></div>
      </div>
    HTML
  else
    <<~HTML
      <div id="bench-result" class="stats">
        <div><div class="stat">#{r[:ops]}</div><div class="stat-label">OPS</div></div>
        <div><div class="stat">#{r[:ms]}<span style="font-size:.8rem;color:#666"> ms</span></div><div class="stat-label">ELAPSED</div></div>
        <div><div class="stat">#{r[:rate]}</div><div class="stat-label">OPS / SEC</div></div>
      </div>
    HTML
  end
end

def render_bench_panel
  <<~HTML
    <div class="panel full">
      <h2>BENCHMARK · LONG-RUNNING RUBY WORK</h2>
      #{render_bench_result(nil)}
      #{render_bench_form}
      <p class="timing" style="margin-top:.8rem">
        Demonstrates rb-indicator while the Ruby block runs.
        The UI stays responsive because webview's bind awaits a JS promise.
      </p>
    </div>
  HTML
end



def render_page(w_bindings)
  <<~HTML
    <!doctype html><html><head><meta charset="utf-8"><title>dashboard</title>
    #{Hypha.html_router(:route)}<style>#{CSS}</style></head>
    <body><h1>MRUBY-WEBVIEW DASHBOARD</h1>
    <div class="grid">
      #{render_webview_panel}
      #{render_mruby_panel}
      #{render_env_panel}
      #{render_bindings_panel(w_bindings)}
      #{render_bench_panel}
    </div>
    </body></html>
  HTML
end

# ---- routing --------------------------------------------------------------

def route(method, path, params)
  case [method, path]
  when ["POST", "/bench"]
    ms = params["ms"].to_i
    ms = 200 if ms < 50
    ms = 5000 if ms > 5000
    render_bench_result(benchmark(ms))
  else
    "<p style='color:crimson'>404 #{method} #{path}</p>"
  end
end


    Hypha.run(title: "dashboard x mruby", size: [900, 720], debug: true) do |w|
      w.bind(:route) { |m, p, params| route(m, p, params) }
      w.html = render_page(w.bindings.map(&:to_s))
    end