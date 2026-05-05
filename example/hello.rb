# Minimal "hello world" example for mruby-webview.
#
# Build mruby with this gem enabled, then run:
#   ./bin/mruby example/hello.rb

html = <<~HTML
  <!doctype html>
  <html>
    <head><meta charset="utf-8"><title>mruby-webview</title></head>
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
s = TCPServer.new 1600
s.listen 1000

Webview.open(title: 'mruby-webview demo', size: [640, 480], debug: true) do |w|
  w.add_native_event(s) do |fd, what|
    client = fd.accept
    client.write "hello"
    client.close
    true
  end

  w.bind(:greet) do |name|
    "Hello, #{name}! (replied at #{Time.now rescue 'now'})"
  end

  w.bind(:quit) { w.terminate }

  w.html = html
end
