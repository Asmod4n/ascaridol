# Ascaridol Ruby-driven native menu example.
#
# Demonstrates `Ascaridol.menu =` — array spec, Tauri-style. Items reference
# Ruby bindings by symbol; native activation invokes the proc directly.
# No HTML scraping, no JS round-trip.
#
# Spec shape:
#   [ [group_label, [item, item, ...]], ... ]
# Items:
#   [label, :bind_sym]                  basic
#   [label, :bind_sym, "CmdOrCtrl+S"]   with accelerator
#   [:separator]                        separator line
#
# `CmdOrCtrl` in accel strings becomes Cmd on macOS, Ctrl elsewhere.

def main
  Ascaridol.run(title: 'Menu demo', size: [800, 600]) do |a|
    a.bind(:open_file)   { puts "[ruby] open file";    nil }
    a.bind(:save_file)   { puts "[ruby] save file";    nil }
    a.bind(:save_as)     { puts "[ruby] save as";      nil }
    a.bind(:greet)       { puts "[ruby] hello";        nil }
    a.bind(:grow_text)   { Ascaridol.eval "document.body.style.fontSize = (parseInt(getComputedStyle(document.body).fontSize) + 2) + 'px'"; nil }
    a.bind(:shrink_text) { Ascaridol.eval "document.body.style.fontSize = Math.max(8, parseInt(getComputedStyle(document.body).fontSize) - 2) + 'px'"; nil }
    a.bind(:about)       { Ascaridol.eval "alert('Ascaridol menu demo\\nPlatform: #{Ascaridol.platform}')"; nil }
    a.bind(:quit)        { Ascaridol.terminate; nil }

    Ascaridol.menu = [
      ["File", [
        ["Open\u2026", :open_file, "CmdOrCtrl+O"],
        ["Save",       :save_file, "CmdOrCtrl+S"],
        ["Save As\u2026", :save_as, "CmdOrCtrl+Shift+S"],
        [:separator],
        ["Quit",       :quit,      "CmdOrCtrl+Q"],
      ]],
      ["Edit", [
        ["Greet",        :greet],
        [:separator],
        ["Bigger text",  :grow_text,   "CmdOrCtrl+Plus"],
        ["Smaller text", :shrink_text, "CmdOrCtrl+Minus"],
      ]],
      ["Help", [
        ["About", :about],
      ]],
    ]

    a.html = <<~HTML
      <!doctype html>
      <html>
        <head>
          <meta charset="utf-8">
          <title>Menu demo</title>
          <style>
            :root { color-scheme: only light; }
            html, body { background: #fff; color: #111; }
            body { font: 16px/1.4 system-ui, sans-serif; margin: 2em; }
            kbd  { background: #eee; border: 1px solid #ccc; border-radius: 3px;
                   padding: 0 4px; font-family: monospace; color: #111; }
            code { color: #111; }
          </style>
        </head>
        <body>
          <h1>Ruby-driven native menu</h1>
          <p>The menu bar above is built from a Ruby array, not scraped from HTML.
            Items map to <code>Ascaridol.bind</code> registrations by symbol;
            activation invokes the proc directly — no JS round-trip.</p>
          <p>Try <kbd>Quit</kbd>, or click around.</p>
          <p>Platform: <code>#{Ascaridol.platform}</code></p>
        </body>
      </html>
    HTML
  end
end