# Minimal "hello world" example for Ascaridol.
#
# Build:
#   conf.gem '<path-to-ascaridol-mrb>' do |ascaridol|
#     ascaridol.rbfiles << '../example/hello.rb'
#   end
#   rake
#
# Run:
#   mruby/build/host/bin/ascaridol
#
# Shows: window creation, html=, bind, calling Ruby from JS,
# and Ascaridol.ready firing once before the run loop starts pumping.

html = <<~HTML
  <!doctype html>
  <html>
    <head><meta charset="utf-8"><title>mruby-webview</title>
    <meta name="color-scheme" content="light dark">
<style>
  @media (prefers-color-scheme: dark) {
    body { background: #1e1e1e; color: #e0e0e0; }
    h1 { color: #fff; }
  }
</style>
    </head>
    <body style="padding: 2em;">
      <h1>Hello from mruby!</h1>
      <p>Click the button to call into Ruby.</p>
      <button id="btn">Greet</button>
      <pre id="out"></pre>
      <p id="status" style="color: gray; font-size: 0.85em;">waiting for ready...</p>
      <script>
        document.getElementById('btn').addEventListener('click', async () => {
          const reply = await window.greet('world');
          document.getElementById('out').textContent = reply;
        });
      </script>
    </body>
  </html>
HTML

def main
  Ascaridol.ready {
    Ascaridol.eval("document.getElementById('status').textContent = 'Ready — fired from Ascaridol.ready'")
    puts "Ascaridol is ready to use"
  }

  Ascaridol.run(title: 'mruby-webview demo', size: [640, 480]) do |w|

    w.bind(:greet) do |name|
      "Hello, #{name}! (replied at #{Time.now rescue 'now'})"
    end

    w.html = html
  end
end
