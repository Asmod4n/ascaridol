# Ascaridol HTML-menu example.
#
# Demonstrates `Ascaridol.enable_html_menu`: write a hidden `<menu>` tree,
# Ascaridol scrapes it into a real native menu bar (Win32, Cocoa, or GTK).
# Item actions are just JS in `data-action`; accelerators go in `data-accel`
# in the platform-native syntax that `Ascaridol.platform` tells you to use.

def main
  # Format an accelerator string in the current platform's native syntax.
  accel = ->(key) {
    case Ascaridol.platform
    when :macos   then "Cmd+#{key}"               # NSEventModifierFlag tokens
    when :windows then "Ctrl+#{key}"              # vkey + Ctrl/Shift/Alt
    else               "<Control>#{key.downcase}" # gtk_accelerator_parse syntax
    end
  }

  Ascaridol.run(title: 'HTML menu demo', size: [800, 600]) do |a|
    # Bindings the menu items will call via data-action.
    a.bind(:say)  { |msg| puts "[ruby] #{msg}"; nil }
    a.bind(:quit) { Ascaridol.terminate; nil }

    # Wire the scraper + native installer. Must be called before the HTML
    # is set so the init script is registered for first page load.
    a.enable_html_menu

    a.html = <<~HTML
      <!doctype html>
      <html>
        <head>
          <meta charset="utf-8">
          <title>HTML menu demo</title>
          <style>
            body { font: 16px/1.4 system-ui, sans-serif; margin: 2em; }
            kbd { background: #eee; border: 1px solid #ccc; border-radius: 3px;
                  padding: 0 4px; font-family: monospace; }
          </style>
        </head>
        <body>
          <!-- Native menu spec. The id is what the scraper looks for; the
               hidden attribute keeps it out of the rendered page. Each
               <menu label="..."> is a top-level group; each <li> is an item.
               data-action is JS (can call Ruby bindings or anything else).
               data-accel uses the current platform's accelerator syntax. -->
          <menu id="ascaridol-menu" hidden>
            <menu label="File">
              <li label="Open\u2026"
                  data-action="alert('Open dialog goes here.')"
                  data-accel="#{accel.('O')}"></li>
              <li label="Save"
                  data-action="say('save clicked')"
                  data-accel="#{accel.('S')}"></li>
              <li label="Quit"
                  data-action="quit()"
                  data-accel="#{accel.('Q')}"></li>
            </menu>

            <menu label="Edit">
              <li label="Greet"
                  data-action="say('hello from the menu')"></li>
              <li label="Bigger text"
                  data-action="document.body.style.fontSize = (parseInt(getComputedStyle(document.body).fontSize) + 2) + 'px'"
                  data-accel="#{accel.('plus')}"></li>
              <li label="Smaller text"
                  data-action="document.body.style.fontSize = Math.max(8, parseInt(getComputedStyle(document.body).fontSize) - 2) + 'px'"
                  data-accel="#{accel.('minus')}"></li>
            </menu>

            <menu label="Help">
              <li label="About"
                  data-action="alert('Ascaridol HTML menu demo\\nPlatform: #{Ascaridol.platform}')"></li>
            </menu>
          </menu>

          <h1>HTML-driven native menu</h1>
          <p>The menu bar at the top is real native chrome — built from the
            hidden <code>&lt;menu id="ascaridol-menu"&gt;</code> in this page.</p>
          <p>Try <kbd>#{accel.('Q')}</kbd>, or click around.</p>
          <p>Platform: <code>#{Ascaridol.platform}</code></p>
        </body>
      </html>
    HTML
  end
end