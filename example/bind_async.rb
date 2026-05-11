Hypha.run(title: "stdin → JS promise", size: [640, 480]) do |h|
  waiters = []
  buffer  = []
  eof     = false

  deliver = lambda do
    loop do
      break if waiters.empty?
      if !buffer.empty?
        Hypha.resolve(waiters.shift) { buffer.shift }
      elsif eof
        Hypha.resolve(waiters.shift) { nil }   # signal EOF to JS
      else
        break
      end
    end
  end

  h.bind_async(:wait_for_line) do |id|
    waiters << id
    deliver.call
  end

  h.add_native_event($stdin) do |io, _events|
    line = io.gets
    if line
      buffer << line.chomp
      deliver.call
      true
    else
      eof = true
      deliver.call    # drain anyone already waiting
      false           # stop watching
    end
  end

  h.html = <<~HTML
    <!doctype html>
    <html><head><style>
      body { font: 15px system-ui; padding: 2rem; max-width: 42rem; }
      button { font-size: 1rem; padding: .5rem 1rem; cursor: pointer; margin-right: .5rem; }
      button:disabled { opacity: .5; cursor: not-allowed; }
      #log { margin-top: 1rem; padding: 1rem; background: #111; color: #c8f542;
             border-radius: 4px; font-family: ui-monospace, monospace;
             min-height: 2.5rem; }
      #log div { padding: .15rem 0; }
      #log .pending { color: #666; }
      #log .eof     { color: crimson; }
    </style></head>
    <body>
      <h1>stdin as a JS promise</h1>
      <p>Click a button, type lines in the terminal. Ctrl+D closes stdin —
         after that, further clicks resolve immediately with EOF.</p>
      <button id="b1" onclick="getOne()">Get next line</button>
      <button id="b3" onclick="getThree()">Get three in a row</button>
      <div id="log"></div>
      <script>
        const log = document.getElementById('log');
        let closed = false;

        function row(text) {
          const d = document.createElement('div');
          d.textContent = text;
          d.className = 'pending';
          log.appendChild(d);
          return d;
        }

        function markClosed() {
          closed = true;
          document.getElementById('b1').disabled = true;
          document.getElementById('b3').disabled = true;
        }

        async function getOne() {
          const r = row('waiting...');
          const line = await wait_for_line();
          if (line === null) {
            r.textContent = 'EOF'; r.className = 'eof'; markClosed();
          } else {
            r.textContent = 'got: ' + line; r.className = '';
          }
        }

        async function getThree() {
          const rows = [1, 2, 3].map(i => row(`(${i}/3) waiting...`));
          for (let i = 0; i < 3; i++) {
            const line = await wait_for_line();
            if (line === null) {
              rows[i].textContent = `(${i + 1}/3) EOF`;
              rows[i].className = 'eof';
              for (let j = i + 1; j < 3; j++) {
                rows[j].textContent = `(${j + 1}/3) skipped (EOF)`;
                rows[j].className = 'eof';
              }
              markClosed();
              break;
            }
            rows[i].textContent = `(${i + 1}/3) got: ` + line;
            rows[i].className = '';
          }
        }
      </script>
    </body></html>
  HTML
end