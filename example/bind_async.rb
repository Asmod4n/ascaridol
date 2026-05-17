# Async bind example for Hypha — Promise resolution from Ruby.
#
# Build:
#   conf.gem '<path-to-hypha-mrb>' do |hypha|
#     hypha.hypha_main = File.expand_path('example/bind_async.rb', __dir__)
#   end
#   rake
#
# Run:
#   mruby/build/host/bin/hypha
#
# Shows: bind_async to return a hanging Promise to JS, then resolve it
# later with Hypha.resolve. Demonstrates FIFO queueing of waiters.

Hypha.run(title: "resolve demo", size: [400, 200]) do |h|
  @pending = []
  @counter = 0

  # JS calls wait_for_ping() and gets a promise that hangs until
  # someone calls ping_now(). Multiple waiters queue up FIFO.
  h.bind_async(:wait_for_ping) do |id|
    @counter += 1
    @pending << [id, @counter]
  end

  h.bind(:ping_now) do
    if (slot = @pending.shift)
      id, n = slot
      h.resolve(id) { "pong ##{n} at #{Time.now.to_f}" }
      "delivered"
    else
      "no one waiting"
    end
  end

  h.html = <<~'HTML'
    <!doctype html>
    <button onclick="wait_for_ping().then(v => log.textContent += v + '\n')">wait</button>
    <button onclick="ping_now()">ping</button>
    <pre id="log"></pre>
  HTML
end