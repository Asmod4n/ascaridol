  Hypha.run(title: "resolve demo", size: [400, 200]) do |h|
    pending = nil

    # JS calls wait_for_ping() and gets a promise that hangs until
    # someone calls ping_now().
    h.bind_async(:wait_for_ping) do |id|
      pending = id
    end

    h.bind(:ping_now) do
      if pending
        id, pending = pending, nil
        h.resolve(id) { "pong at #{Time.now.to_i}" }
        "delivered"
      else
        "no one waiting"
      end
    end

    h.html = '<!doctype html>' \
             '<button onclick="wait_for_ping().then(v => log.textContent = v)">wait</button>' \
             '<button onclick="ping_now()">ping</button>' \
             '<pre id="log"></pre>'
  end