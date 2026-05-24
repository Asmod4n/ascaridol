# Hacker News live feed — Ascaridol showcase example.
#
# Demonstrates:
#   - Server-Sent Events over a persistent curl connection (Ascaridol::EventLoop)
#   - add_native_event driving curl's socket callbacks
#   - add_timer for curl timeout + exponential reconnect backoff
#   - URL.parallel for fetching story details
#   - LMDB persistence (stories survive restart)
#   - HTMX router for UI interactions
#   - Native menu (pause/resume, open in browser, clear history)
#
# Build:
#   conf.gem '<path-to-ascaridol-mrb>' do |ascaridol|
#     ascaridol.rbfiles << '../example/hn_live.rb'
#   end

HN_SSE_URL   = "https://hacker-news.firebaseio.com/v0/newstories.json"
HN_ITEM_URL  = "https://hacker-news.firebaseio.com/v0/item/%d.json"
HN_STORY_URL = "https://news.ycombinator.com/item?id=%d"
HN_ITEM_SSE  = "https://hacker-news.firebaseio.com/v0/item/%d.json"
DB_DIR        = "./ascaridol-hn"
FETCH_COUNT   = 30
RECONNECT_CAP = 30_000
MAX_SSE       = 10
POLL_INTERVAL_S = 30
QUIET_TIMEOUT_S = 120

CSS = <<~'CSS'
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: Verdana, Geneva, sans-serif;
    font-size: 10pt;
    background: #f6f6ef;
    color: #000;
    display: flex; flex-direction: column; height: 100vh; overflow: hidden;
  }
  header {
    background: #ff6600;
    padding: 2px 6px;
    display: flex; align-items: center; gap: 8px;
    flex-shrink: 0;
  }
  header h1 {
    font-size: 10pt; font-weight: bold; color: #000;
  }
  header h1 a { color: #000; text-decoration: none; }
  #status {
    font-size: 8pt;
    padding: 1px 6px;
    background: #fff;
    border: 1px solid #000;
    color: #000;
  }
  #status.connected    { color: #006400; }
  #status.reconnecting { color: #aa5500; }
  #status.paused       { color: #555; }
  #count { font-size: 8pt; color: #000; margin-left: auto; }
  #stories {
    flex: 1; overflow-y: auto;
    background: #f6f6ef;
    padding: 8px 10px;
  }
  .story {
    padding: 2px 0 4px 0;
    animation: fadein .3s ease-out;
  }
  @keyframes fadein {
    from { opacity: 0; transform: translateY(-2px); }
    to   { opacity: 1; transform: none; }
  }
  .story-title {
    font-size: 10pt; color: #000;
    cursor: pointer; background: none; border: none;
    font-family: inherit; text-align: left; padding: 0;
  }
  .story-title:hover { text-decoration: underline; }
  .story-title:visited { color: #828282; }
  .story-meta {
    font-size: 7pt; color: #828282;
    display: flex; gap: 4px;
    margin-top: 1px;
  }
  .story-meta span:not(:last-child)::after { content: " |"; margin-left: 2px; }
  .comments-link {
    font: inherit; font-size: 7pt; color: #828282;
    cursor: pointer; background: none; border: none; padding: 0;
  }
  .comments-link:hover { text-decoration: underline; color: #000; }
  .placeholder { color: #828282; font-size: 9pt; padding: 1rem 0; text-align: center; }
CSS

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def open_in_browser(url)
  case Ascaridol.platform
  when :windows then IO.popen("cmd /c start \"\" \"#{url}\"")
  when :macos   then IO.popen("open #{url}")
  else               IO.popen("xdg-open #{url}")
  end
rescue => e
  $stderr.puts "open_in_browser: #{e.message}"
end

def time_ago(t)
  return "?" unless t
  d = Time.now.to_i - t
  case d
  when 0..59       then "#{d}s ago"
  when 60..3599    then "#{d / 60}m ago"
  when 3600..86399 then "#{d / 3600}h ago"
  else                  "#{d / 86400}d ago"
  end
end

def story_html(story)
  id    = story["id"]
  title = (story["title"] || "(untitled)").gsub('"', '&quot;').gsub('<', '&lt;').gsub('>', '&gt;')
  score = story["score"] || 0
  by    = story["by"]    || "unknown"
  kids  = story["descendants"] || 0
  url   = story["url"]   || (HN_STORY_URL % id)
  url_e = URI.encode(url)

  <<~HTML
    <div class="story" id="story-#{id}">
      <button class="story-title"
        rb-post="/open/#{id}?url=#{url_e}"
        rb-target="#story-#{id}">#{title}</button>
      <div class="story-meta">
        <span>&#9650; #{score}</span>
        <span>#{by}</span>
        <span class="story-time" data-time="#{story["time"]}">#{time_ago(story["time"])}</span>
        <button class="comments-link"
          rb-post="/comments/#{id}"
          rb-target="#story-#{id}">#{kids} comments</button>
      </div>
    </div>
  HTML
end

# ---------------------------------------------------------------------------
# SSE chunk accumulator
# ---------------------------------------------------------------------------

class SSEParser
  def initialize
    @buf = ""
  end

  def feed(chunk, &blk)
    @buf << chunk
    while (pos = @buf.index("\n\n"))
      block = @buf.slice!(0, pos + 2)
      ev = {}
      block.each_line do |line|
        line.chomp!
        ev[:event] = line[6..].strip if line.start_with?("event:")
        ev[:data]  = line[5..].strip if line.start_with?("data:")
      end
      blk.call(ev) if blk && !ev.empty?
    end
  end
end

# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------

class HNStore
  def initialize(dir)
    @env = MDB::Env.new(mapsize: 64 * 1024 * 1024, maxdbs: 4)
    @env.open(dir, MDB::NOSUBDIR)
    # Primary: id => CBOR(story).  INTEGERKEY = native-endian fixed-width int.
    @by_id   = @env.database(MDB::CREATE | MDB::INTEGERKEY, "stories")
    # Secondary: (time:32 | id:32) packed little-endian => id.to_bin.
    # Cursor walking backward over this gives newest-first by time, with id
    # breaking ties.  INTEGERKEY again — keys are 8-byte fixed-width.
    @by_time = @env.database(MDB::CREATE | MDB::INTEGERKEY, "by_time")
  end

  def close
    @env.close rescue nil
  end

  def save(story)
    id   = story["id"]   or return
    time = story["time"] or return
    # If this id was already stored, remove its old time-index entry first.
    if old = load(id)
      old_time = old["time"].to_i
      @by_time.del(time_key(old_time, id)) rescue nil
    end
    @by_id[id.to_bin]                  = CBOR.encode(story)
    @by_time[time_key(time.to_i, id)]  = id.to_bin
  end

  def load(id)
    raw = @by_id[id.to_bin] rescue nil
    raw ? CBOR.decode(raw) : nil
  end

  def has?(id)
    !@by_id.fetch(id.to_bin, nil).nil?
  end

  # Newest N stories by submission time. Cursor walks @by_time in reverse;
  # each value is the id.to_bin for the primary lookup.
  def newest(n)
    out = []
    @by_time.cursor do |c|
      pair = c.last
      while pair && out.size < n
        _k, id_bin = pair
        raw = @by_id[id_bin] rescue nil
        out << CBOR.decode(raw) if raw
        pair = c.prev
      end
    end
    out
  end

  def count
    @by_id.length
  end

  def clear
    @by_id.drop(false)
    @by_time.drop(false)
  end

  private

  # Pack (time, id) into a single 64-bit little-endian key.
  # time goes in the high 32 bits, id in the low 32 bits.
  # mruby-lmdb sorts INTEGERKEY numerically, so larger (= newer) sorts last;
  # walking with cursor.last + cursor.prev yields newest-first.
  def time_key(time, id)
    ((time & 0xFFFFFFFF) << 32 | (id & 0xFFFFFFFF)).to_bin
  end
end

# ---------------------------------------------------------------------------
# App — owns ALL state. main() just calls App.new.start.
# ---------------------------------------------------------------------------

class App
  def initialize
    @store        = HNStore.new(DB_DIR)
    @parser       = SSEParser.new            # parser for the main SSE feed
    @pending      = []                       # work deferred out of curl callbacks
    @subs         = {}                       # id => { mode:, parser:, last_event: }
    @paused       = false
    @reconnect_ms = 1_000
  end

  def start
    Ascaridol.ready { on_ready }
    Ascaridol.run(title: "HN Live", size: [720, 800]) do
      install_bindings
      install_menu
      install_html
    end
    @store.close
  end

  # ----- Lifecycle ----------------------------------------------------------

  def on_ready
    URL.shared.event_loop = Ascaridol::EventLoop.new(URL.shared)
    rehydrate
    connect
    Ascaridol.add_timer(1.s)              { refresh_times }
    Ascaridol.add_timer(POLL_INTERVAL_S.s) { poll_cycle;   true }
    Ascaridol.add_timer(1.min)             { quiet_sweep;  true }
    nil
  end

  # ----- Main SSE feed ------------------------------------------------------

  def connect
    return if @paused
    Ascaridol.eval("document.getElementById('status').className='connected';document.getElementById('status').textContent='CONNECTED'")

    URL.get(HN_SSE_URL,
      follow_location: true,
      timeout_ms:      0,
      headers:         { "Accept" => "text/event-stream" }
    ) do |chunk|
      @parser.feed(chunk) do |ev|
        next unless ev[:event] == "put" && ev[:data]
        parsed = JSON.parse(ev[:data]) rescue next
        ids = parsed["data"]
        next unless ids.is_a?(Array)

        new_ids = ids.first(FETCH_COUNT).reject { |id| @store.has?(id) }
        next if new_ids.empty?

        @pending << lambda do
          new_ids.each_with_index do |id, idx|
            if idx < MAX_SSE
              promote_to_sse(id)
            else
              fetch_once(id)
            end
          end
        end
        Ascaridol.add_timer(0.ms) { drain; false }
      end
    end
  end

  # ----- Per-story subscriptions -------------------------------------------

  def promote_to_sse(id)
    return if @subs[id] && @subs[id][:mode] == :sse

    sse_subs = @subs.select { |_id, s| s[:mode] == :sse }
    if sse_subs.size >= MAX_SSE
      stale_id, _ = sse_subs.to_a.sort { |a, b|
        (a[1][:last_event] || 0) <=> (b[1][:last_event] || 0)
      }.first
      demote_to_poll(stale_id) if stale_id
    end

    parser = SSEParser.new
    @subs[id] = { mode: :sse, parser: parser, last_event: Time.now.to_i }

    URL.get(HN_ITEM_URL % id,
      follow_location: true,
      timeout_ms:      0,
      headers:         { "Accept" => "text/event-stream" }
    ) do |chunk|
      sub = @subs[id]
      next unless sub && sub[:mode] == :sse
      parser.feed(chunk) do |ev|
        next unless ev[:event] == "put" || ev[:event] == "patch"
        next unless ev[:data] && ev[:data] != "null"
        parsed = JSON.parse(ev[:data]) rescue next
        s = parsed.is_a?(Hash) ? parsed["data"] : nil
        next unless s.is_a?(Hash)
        s["id"] ||= id
        @pending << lambda { apply_update(s) }
        Ascaridol.add_timer(0.ms) { drain; false }
      end
    end
  end

  def demote_to_poll(id)
    sub = @subs[id] or return
    return if sub[:mode] == :poll
    sub[:mode]   = :poll
    sub[:parser] = nil
  end

  # One-shot fetch + store; mark as :poll so future updates come via polling.
  def fetch_once(id)
    return if @subs[id]
    @subs[id] = { mode: :poll, parser: nil, last_event: 0 }
    buf = String.new
    URL.get(HN_ITEM_URL % id) do |chunk|
      buf << chunk
      s = JSON.parse(buf) rescue next
      next unless s.is_a?(Hash) && s["type"] == "story"
      @pending << lambda { apply_update(s) }
      Ascaridol.add_timer(0.ms) { drain; false }
    end
  end

  def apply_update(s)
    id = s["id"] or return
    old = @store.load(id)
    return if old &&
              old["score"]       == s["score"] &&
              old["descendants"] == s["descendants"]
    @store.save(s)
    if sub = @subs[id]
      sub[:last_event] = Time.now.to_i
    end
    render_story(s)
  end

  def poll_cycle
    @subs.each do |id, sub|
      next unless sub[:mode] == :poll
      buf = String.new
      URL.get(HN_ITEM_URL % id) do |chunk|
        buf << chunk
        s = JSON.parse(buf) rescue next
        next unless s.is_a?(Hash) && s["type"] == "story"
        old = @store.load(id)
        next unless !old ||
                    old["score"]       != s["score"] ||
                    old["descendants"] != s["descendants"]
        apply_update(s)
        newest_ids = @store.newest(MAX_SSE).map { |x| x["id"] }
        promote_to_sse(id) if newest_ids.include?(id)
      end
    end
  end

  def quiet_sweep
    now = Time.now.to_i
    @subs.each do |id, sub|
      next unless sub[:mode] == :sse
      next unless sub[:last_event] && (now - sub[:last_event]) > QUIET_TIMEOUT_S
      demote_to_poll(id)
    end
  end

  def rehydrate
    newest = @store.newest(FETCH_COUNT)
    newest.first(MAX_SSE).each { |s| promote_to_sse(s["id"]) }
    newest[MAX_SSE..]&.each do |s|
      @subs[s["id"]] ||= { mode: :poll, parser: nil, last_event: 0 }
    end
  end

  def drain
    until @pending.empty?
      op = @pending.shift
      begin
        op.call
      rescue => e
        $stderr.puts "pending op error: #{e.class}: #{e.message}"
      end
    end
  end

  # ----- Rendering ----------------------------------------------------------

  def render_story(s)
    return unless s && s["id"]
    html_one = story_html(s)
    escaped  = html_one.gsub("\\", "\\\\\\\\").gsub("`", "\\`").gsub("$", "\\$")
    Ascaridol.eval(<<~JS)
      (function(){
        var id   = "#{s["id"]}";
        var time = #{s["time"].to_i};
        var t = document.createElement("div");
        t.innerHTML = `#{escaped}`;
        var fresh = t.firstChild;
        var el = document.getElementById("stories");
        var ph = el.querySelector(".placeholder");
        if (ph) ph.remove();

        // If a row for this id already exists, replace it in place.
        var existing = document.getElementById("story-" + id);
        if (existing) {
          existing.parentNode.replaceChild(fresh, existing);
          document.getElementById("count").textContent = "#{@store.count} stories";
          return;
        }

        // Otherwise insert at the position matching time order (newest first).
        // Find the first sibling whose time is <= ours and insert before it.
        var children = el.children;
        var inserted = false;
        for (var i = 0; i < children.length; i++) {
          var child   = children[i];
          var timeEl  = child.querySelector(".story-time");
          if (!timeEl) continue;
          var t2 = parseInt(timeEl.dataset.time, 10);
          if (isNaN(t2)) continue;
          if (time >= t2) {
            el.insertBefore(fresh, child);
            inserted = true;
            break;
          }
        }
        if (!inserted) el.appendChild(fresh);
        document.getElementById("count").textContent = "#{@store.count} stories";
      })();
    JS
  end

  def refresh_times
    Ascaridol.eval(<<~JS)
      (function(){
        var now = Math.floor(Date.now()/1000);
        document.querySelectorAll('.story-time').forEach(function(el) {
          var t = parseInt(el.dataset.time, 10);
          if (!t) return;
          var d = now - t;
          if (d < 0) d = 0;
          el.textContent = d < 60 ? d + 's ago'
                         : d < 3600 ? Math.floor(d/60) + 'm ago'
                         : d < 86400 ? Math.floor(d/3600) + 'h ago'
                         : Math.floor(d/86400) + 'd ago';
        });
      })();
    JS
    true  # always rearm at the same 1s interval
  end

  # ----- Bindings / Menu / HTML --------------------------------------------

  def install_bindings
    Ascaridol.bind(:pause_stream) do
      @paused = true
      Ascaridol.eval("document.getElementById('status').className='paused';document.getElementById('status').textContent='PAUSED'")
    end

    Ascaridol.bind(:resume_stream) do
      @paused = false
      @reconnect_ms = 1_000
      connect
    end

    Ascaridol.bind(:clear_history) do
      @store.clear
      @subs.clear
      Ascaridol.eval("document.getElementById('stories').innerHTML='<p class=\"placeholder\">Cleared.</p>'")
      Ascaridol.eval("document.getElementById('count').textContent='0 stories'")
    end

    Ascaridol.bind(:open_hn) { open_in_browser("https://news.ycombinator.com") }

    Ascaridol.bind(:route) do |method, path, _params|
      uri     = URI.parse("https://localhost#{path}")
      bare    = uri.path
      qparams = uri.query_hash || {}

      if method == "POST" && bare.start_with?("/open/")
        id  = bare[6..].to_i
        url = qparams["url"] || (HN_STORY_URL % id)
        open_in_browser(url)
        story = @store.load(id)
        story ? story_html(story) : ""
      elsif method == "POST" && bare.start_with?("/comments/")
        id = bare[10..].to_i
        open_in_browser(HN_STORY_URL % id)
        story = @store.load(id)
        story ? story_html(story) : ""
      else
        "<p style='color:crimson'>404 #{method} #{path}</p>"
      end
    end

    Ascaridol.bind(:page_loaded) do
      html = @store.newest(FETCH_COUNT).map { |s| story_html(s) }.join
      html = "<p class='placeholder'>Waiting for stories\u2026</p>" if html.empty?
      escaped = html.gsub("\\", "\\\\\\\\").gsub("`", "\\`").gsub("$", "\\$")
      Ascaridol.eval(<<~JS)
        document.getElementById('stories').innerHTML = `#{escaped}`;
        document.getElementById('count').textContent = '#{@store.count} stories';
      JS
      nil
    end
  end

  def install_menu
    Ascaridol.menu = [
      ["Stream", [
        ["Pause",         :pause_stream],
        ["Resume",        :resume_stream],
        [:separator],
        ["Clear history", :clear_history],
      ]],
      ["View", [
        ["Open HN", :open_hn],
      ]],
    ]
  end

  def install_html
    Ascaridol.html = <<~HTML
      <!doctype html>
      <html>
      <head>
        <meta charset="utf-8">
        <style>#{CSS}</style>
        #{Ascaridol.html_router(:route)}
      </head>
      <body>
        <header>
          <h1>HN LIVE</h1>
          <span id="status" class="reconnecting">CONNECTING</span>
          <span id="count">0 stories</span>
        </header>
        <div id="stories"><p class="placeholder">Loading\u2026</p></div>
        <script>
          document.addEventListener('DOMContentLoaded', function(){
            window.page_loaded();
          });
        </script>
      </body>
      </html>
    HTML
  end
end

def main
  App.new.start
end