# example/echo_server.rb
#
# TCP echo server demonstrating Hypha.poll_add and Watcher#update.
# The watcher toggles between :r (read only) and :rw (read + write)
# based on outbox state, so we only get write-ready wakeups when there
# are actually bytes queued to send.
#
#   $ hypha example/echo_server.rb
#   $ nc 127.0.0.1 5000           # another terminal
#
# (On Windows, ncat from nmap works; PowerShell has no built-in nc.)

Hypha.run(title: "Echo Server", size: [460, 400]) do
  Hypha.html = <<~HTML
    <!doctype html>
    <meta charset="utf-8">
    <meta name="color-scheme" content="light">
    <style>
      html, body { margin: 0; height: 100%; box-sizing: border-box; }
      body {
        font: 14px system-ui, -apple-system, Segoe UI, sans-serif;
        background: #ffffff;
        color: #222;
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 8px;
      }
      h2 { margin: 0; font-size: 16px; }
      p  { margin: 0; color: #666; }
      code {
        background: #eee; color: #222;
        padding: 1px 5px; border-radius: 3px;
        font-family: ui-monospace, Consolas, monospace;
      }
      #log {
        flex: 1; margin: 0; padding: 10px;
        background: #1e1e1e; color: #d4d4d4;
        font: 12px/1.5 ui-monospace, Consolas, monospace;
        border-radius: 4px;
        overflow: auto;
        white-space: pre-wrap;
      }
    </style>
    <h2>Echo server on 127.0.0.1:5000</h2>
    <p>try <code>nc 127.0.0.1 5000</code></p>
    <pre id="log"></pre>
  HTML

  log = ->(line) {
    Hypha.eval(%(document.getElementById('log').textContent += #{line.dump} + "\\n";
                 var el = document.getElementById('log'); el.scrollTop = el.scrollHeight;))
  }

  Hypha.ready {
      puts "Echo server running"
  }

  server = TCPServer.new("127.0.0.1", 5000)
  server._setnonblock(true)        # accepted clients inherit this flag

  # Listener: only ever readable (incoming connections), so :r is enough.
  Hypha.poll_add(server, :r) do |srv, _cond|
    client = srv.accept
    outbox = String.new
    wants_write = false
    log.call("+ connected")

    # We need a reference to the watcher inside its own callback so we
    # can toggle :r <-> :rw. The closure captures `watcher` by name, so
    # the assignment below resolves correctly by the time the block runs.
    watcher = nil
    watcher = Hypha.poll_add(client, :r) do |sock, cond|
      alive = true

      # Read side -- :r and :rw both signal readable.
      if cond == :r || cond == :rw
        begin
          chunk = sock.recv(4096)
          if chunk.nil? || chunk.empty?
            alive = false
          else
            outbox << chunk
            log.call("> #{chunk.chomp}")
          end
        rescue Errno::EAGAIN, Errno::EWOULDBLOCK
          # spurious -- keep watching
        rescue EOFError, Errno::ECONNRESET, IOError
          alive = false
        end
      elsif cond == :err
        alive = false
      end

      # Write side -- attempt drain whenever the outbox has bytes. Most
      # sends succeed inline because the kernel send buffer almost always
      # has room; EAGAIN handles the rare backpressure case and leaves
      # the bytes queued for the next wakeup.
      if alive && !outbox.empty?
        begin
          n = sock.send(outbox, 0)
          log.call("< #{outbox.byteslice(0, n).chomp}")
          outbox = outbox.byteslice(n..-1) || String.new
        rescue Errno::EAGAIN, Errno::EWOULDBLOCK
          # buffer full -- need :w wakeups until it drains
        rescue Errno::EPIPE, Errno::ECONNRESET, IOError
          alive = false
        end
      end

      # Toggle write interest only when desired state actually changes,
      # so we don't burn syscalls on every callback.
      if alive
        need_write = !outbox.empty?
        if need_write != wants_write
          watcher.update(need_write ? :rw : :r)
          wants_write = need_write
        end
      end

      unless alive
        sock.close rescue nil
        log.call("- disconnected")
      end
      alive
    end

    true
  end
end