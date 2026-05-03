# Minimal "hello world" example for mruby-webview.
#
# Build mruby with this gem enabled, then run:
#   ./bin/mruby example/hello.rb

html = <<~HTML
  <!doctype html>
  <html>
    <head><meta charset="utf-8"><title>mruby-webview</title></head>
    <body style="font-family: sans-serif; padding: 2em;">
      <h1>Hello from mruby!</h1>
      <p>Click the button to call into Ruby.</p>
      <button id="btn">Greet</button>
      <pre id="out"></pre>
      <script>
        document.getElementById('btn').addEventListener('click', async () => {
          const reply = await window.greet('world');
          document.getElementById('out').textContent = reply;
        });
      </script>
    </body>
  </html>
HTML

Webview.open(title: 'mruby-webview demo', size: [640, 480, :none], debug: true) do |w|
  w.bind(:greet) do |name|
    "Hello, #{name}! (replied at #{Time.now rescue 'now'})"
  end

  w.bind(:quit) { w.terminate }

  w.html = html
end
