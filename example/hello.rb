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

s = TCPServer.new 1600
s.listen 1000

Webview.open(title: 'mruby-webview demo', size: [640, 480], debug: true) do |w|
  w.add_native_event(s) do |fd, _what|
    loop do
      client = begin
        fd.accept
      rescue => e
        nil
      end
      break unless client

      body = "hello"
      client.write(
        "HTTP/1.1 200 OK\r\n" \
        "Content-Type: text/html; charset=utf-8\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n" \
        "\r\n" \
        "#{body}"
      )
      client.close
    end
    true   # keep the watcher armed
  end
  w.bind(:greet) do |name|
    "Hello, #{name}! (replied at #{Time.now rescue 'now'})"
  end

  w.bind(:quit) { w.terminate }

  w.html = html
end
