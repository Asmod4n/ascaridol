# Minimal "hello world" example for mruby-webview.
#
# Build mruby with this gem enabled, then run:
#   ./bin/mruby example/hello.rb

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
      <script>
        document.getElementById('btn').addEventListener('click', async () => {
          const reply = await window.greet('world');
          document.getElementById('out').textContent = reply;
        });
      </script>
    </body>
  </html>
HTML

Hypha.run(title: 'mruby-webview demo', size: [640, 480], debug: true) do |w|

  w.bind(:greet) do |name|
    "Hello, #{name}! (replied at #{Time.now rescue 'now'})"
  end

  w.html = html
end
