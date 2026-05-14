# example/echo_server.rb
#
# TCP echo server demonstrating Hypha.add_native_event's readiness argument.
# Listener uses :r (only ever accepts), echo clients use :rw (read + write
# from the same callback).
#
#   $ hypha example/echo_server.rb
#   $ nc 127.0.0.1 5000           # another terminal
#
# (On Windows, ncat from nmap works; PowerShell has no built-in nc.)

# Condition bits yielded to the block. Match GIOCondition values so they're
# identical on GTK, Cocoa, and Win32.

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

  server   = TCPServer.new("127.0.0.1", 5000)
  server._setnonblock(true)        # accepted clients inherit this flag
  outboxes = {}                    # sock => bytes still to send

  # Listener: only ever readable (incoming connections), so :r is enough.
  Hypha.add_native_event(server, :r) do |srv, _conds|
    client = srv.accept
    outboxes[client] = String.new
    log.call("+ connected")

    # Echo client: :rw so one callback handles both directions. A heavier
    # server would toggle :r <-> :rw via remove_native_event + add to avoid
    # writable wakeups when the outbox is empty; for a demo it's fine.
    Hypha.add_native_event(client, :rw) do |sock, conds|
      if conds == :err
        outboxes.delete(sock)
        sock.close rescue nil
        log.call("! error")
        next false
      end

      alive = true

      if conds == :r || conds == :rw
        begin
          chunk = sock.recv(4096)
          if chunk.nil? || chunk.empty?
            alive = false
          else
            outboxes[sock] << chunk
            log.call("> #{chunk.chomp}")
          end
        rescue Errno::EAGAIN, Errno::EWOULDBLOCK
          # spurious -- keep watching
        rescue EOFError, Errno::ECONNRESET, IOError
          alive = false
        end
      end

      # Try to drain the outbox whenever there's something to send. On the
      # first turn after a read, `conds` won't have HYPHA_WRITE set but the
      # buffer almost always has room, so the send succeeds inline. If it
      # would block, EAGAIN leaves the bytes queued and the next :rw wakeup
      # picks them up.
      if alive && !outboxes[sock].empty?
        begin
          n = sock.send(outboxes[sock], 0)
          log.call("< #{outboxes[sock].byteslice(0, n).chomp}")
          outboxes[sock] = outboxes[sock].byteslice(n..-1) || String.new
        rescue Errno::EAGAIN, Errno::EWOULDBLOCK
          # still full -- :rw will refire when room opens up
        rescue Errno::EPIPE, Errno::ECONNRESET, IOError
          alive = false
        end
      end

      unless alive
        outboxes.delete(sock)
        sock.close rescue nil
        log.call("- disconnected")
      end
      alive
    end

    true
  end
end