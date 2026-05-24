
def main
  count = 0
  Ascaridol.ready do
    Ascaridol.add_timer(1000) do
      count += 1
      Ascaridol.eval("document.getElementById('counter').textContent = #{count}")
      true
    end
  end

  Ascaridol.run(title: "Timer", size: [400, 300]) do |a|

    a.html = <<~HTML
      <!DOCTYPE html>
      <html>
      <head>
      <meta charset="utf-8">
      <style>
        body {
          font-family: system-ui, sans-serif;
          background: #1e1e2e;
          color: #cdd6f4;
          display: flex;
          flex-direction: column;
          align-items: center;
          justify-content: center;
          height: 100vh;
          margin: 0;
        }
        #counter {
          font-size: 4rem;
          font-weight: bold;
          color: #89b4fa;
        }
        p { color: #a6adc8; font-size: 0.9rem; margin-top: 0.5rem; }
      </style>
      </head>
      <body>
        <div id="counter">0</div>
        <p>seconds elapsed</p>
      </body>
      </html>
    HTML
  end
end