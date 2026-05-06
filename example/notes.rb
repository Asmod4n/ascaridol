# A persistent note-taking app on top of mruby-webview + mruby-lmdb + mruby-cbor.
#
# Run with:  ./bin/mruby examples/notes/app.rb
#
# Required mgems in build_config.rb:
#   conf.gem github: 'asmod4n/mruby-webview'
#   conf.gem github: 'asmod4n/mruby-lmdb'
#   conf.gem github: 'asmod4n/mruby-cbor'
#
# Demonstrates:
#   - A real, persistent app: notes survive restarts
#   - LMDB for storage, CBOR for the on-disk note format
#   - Debounced search-as-you-type via rb-trigger="input changed delay:200ms"
#   - Master/detail layout where edits round-trip through Ruby
#   - Optimistic title updates with full-form re-render on save

# LMDB store directory. Edit this constant if you want it elsewhere — mruby
# doesn't always ship ENV/Dir builtins, so we keep this dead-simple and
# resolve relative to the working directory.
DB_DIR = "./mruby-webview-notes"

# ---- storage layer --------------------------------------------------------

class NoteStore
  def initialize(dir)
    @env = MDB::Env.new(mapsize: 64 * 1024 * 1024, maxdbs: 4)
    @env.open(dir, MDB::NOSUBDIR)
    @db = @env.database(MDB::CREATE, "notes")
    seed_if_empty
  end

  # Full scan via the fast-path .each iterator. Each value is a CBOR blob.
  def all
    notes = []
    @db.each { |k, v| notes << decode(k, v) }
    notes.sort_by { |n| -n[:updated_at] }
  end

  # db[key] auto-wraps a read txn; returns nil on MDB::NOTFOUND.
  def get(id)
    raw = @db[id] rescue nil
    raw ? decode(id, raw) : nil
  end

  def put(id, title:, body:)
    rec = { "title" => title, "body" => body, "updated_at" => (Time.now.to_i rescue 0) }
    blob = CBOR.encode(rec)
    @db[id] = blob
    decode(id, blob)
  end

  def delete(id)
    @db.del(id) rescue nil
  end

  def new_id
    "n_#{(Time.now.to_f * 1000).to_i rescue rand(1 << 32)}"
  end

  private

  def decode(id, raw)
    h = CBOR.decode(raw)
    {
      id:         id,
      title:      h["title"].to_s,
      body:       h["body"].to_s,
      updated_at: h["updated_at"].to_i,
    }
  end

  def seed_if_empty
    return unless @db.empty?
    put("n_seed_1", title: "Welcome", body: "This note is stored in LMDB as CBOR.\nEdit me, or hit + to add a new note.")
    put("n_seed_2", title: "Search",  body: "Try typing in the search box — search runs in Ruby.")
  end
end

# Allow the demo UI to render even on builds without lmdb / cbor.
HAS_DEPS = Object.const_defined?(:MDB) && Object.const_defined?(:CBOR)
$store = HAS_DEPS ? NoteStore.new(DB_DIR) : nil

# ---- htmx router ----------------------------------------------------------

MRUBY_ROUTER_EXT = <<~'JS'
  <script>
    (function () {
      var SEL = '[rb-get],[rb-post],[rb-put],[rb-patch],[rb-delete]';
      var VERBS = ['get', 'post', 'put', 'patch', 'delete'];
      function methodAndPath(elt) {
        for (var i = 0; i < VERBS.length; i++) {
          if (elt.hasAttribute('rb-' + VERBS[i])) {
            return { method: VERBS[i].toUpperCase(),
                     path: elt.getAttribute('rb-' + VERBS[i]) };
          }
        }
      }
      function dispatch(elt, e) {
        if (e) e.preventDefault();
        var mp = methodAndPath(elt); if (!mp) return;
        var params = {};
        var raw = elt.getAttribute('rb-vals');
        if (raw) try { Object.assign(params, JSON.parse(raw)); } catch (_) {}
        if (elt.tagName === 'FORM') {
          new FormData(elt).forEach(function (v, k) { params[k] = v; });
        } else if (elt.name) {
          params[elt.name] = elt.value;
        }
        var targetSel = elt.getAttribute('rb-target');
        var target    = targetSel ? document.querySelector(targetSel) : elt;
        var swap      = elt.getAttribute('rb-swap') || 'innerHTML';
        if (!target) return;
        window.htmx_route(mp.method, mp.path, params).then(function (html) {
          if (swap === 'outerHTML') target.outerHTML = html;
          else if (swap === 'beforeend') target.insertAdjacentHTML('beforeend', html);
          else target.innerHTML = html;
        }).catch(function (err) {
          console.error('[mruby-router]', err && err.message || err);
        });
      }
      function parseTriggers(raw, defaultEv) {
        return (raw || defaultEv).split(/\s*,\s*/).map(function (t) {
          var parts = t.trim().split(/\s+/);
          var ev = parts.shift();
          var delay = 0;
          parts.forEach(function (p) {
            var m = /^delay:(\d+)(ms|s)?$/.exec(p);
            if (m) delay = parseInt(m[1], 10) * (m[2] === 's' ? 1000 : 1);
          });
          return { ev: ev, delay: delay };
        });
      }
      function wire(root) {
        root.querySelectorAll(SEL).forEach(function (elt) {
          if (elt.__rb_wired) return;
          elt.__rb_wired = true;
          var defaultEv = elt.tagName === 'FORM' ? 'submit' :
                          (elt.tagName === 'INPUT' || elt.tagName === 'TEXTAREA') ? 'input' :
                          'click';
          parseTriggers(elt.getAttribute('rb-trigger'), defaultEv).forEach(function (t) {
            if (t.ev === 'changed') return;     // bare "changed" is a modifier in htmx
            var timer = null;
            elt.addEventListener(t.ev, function (e) {
              if (t.delay) {
                clearTimeout(timer);
                timer = setTimeout(function () { dispatch(elt, e); }, t.delay);
              } else {
                dispatch(elt, e);
              }
            });
          });
        });
      }
      document.addEventListener('DOMContentLoaded', function () { wire(document); });
      new MutationObserver(function (muts) {
        muts.forEach(function (m) { m.addedNodes.forEach(function (n) {
          if (n.nodeType === 1) wire(n);
        }); });
      }).observe(document.documentElement, { childList: true, subtree: true });
    })();
  </script>
JS

CSS = <<~'CSS'
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; }
  body { font-family: 'Courier New', monospace; background: #0f0f0f; color: #e8e8e8;
         display: grid; grid-template-rows: auto 1fr; }
  header { padding: 1rem 1.5rem; border-bottom: 1px solid #2a2a2a;
           display: flex; align-items: center; gap: 1rem; }
  header h1 { font-size: .9rem; color: #888; letter-spacing: .2em; flex: 0 0 auto; }
  header input.search {
    flex: 1; max-width: 320px; background: #111; border: 1px solid #2a2a2a;
    color: #e8e8e8; font-family: inherit; font-size: .85rem; padding: .5rem .8rem;
    border-radius: 3px; outline: none;
  }
  header input.search:focus { border-color: #444; }
  header button {
    cursor: pointer; border: 1px solid #c8f542; background: transparent; color: #c8f542;
    padding: .5rem 1rem; border-radius: 3px; font-family: inherit; font-size: .8rem;
    letter-spacing: .15em;
  }
  header button:hover { background: #c8f54218; }
  .layout { display: grid; grid-template-columns: 280px 1fr; min-height: 0; }
  .list { border-right: 1px solid #2a2a2a; overflow-y: auto; }
  .item {
    padding: .85rem 1.2rem; border-bottom: 1px solid #1c1c1c; cursor: pointer;
  }
  .item:hover { background: #161616; }
  .item.active { background: #1a1a1a; border-left: 2px solid #c8f542; padding-left: calc(1.2rem - 2px); }
  .item .t { font-size: .85rem; color: #e8e8e8; margin-bottom: .25rem;
             white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .item .p { font-size: .75rem; color: #666; white-space: nowrap; overflow: hidden;
             text-overflow: ellipsis; }
  .empty-list { padding: 2rem; color: #555; text-align: center; font-size: .8rem;
                letter-spacing: .15em; }
  .editor { display: grid; grid-template-rows: auto 1fr auto; min-height: 0; }
  .editor .ti, .editor textarea {
    background: #0f0f0f; border: none; outline: none; color: #e8e8e8;
    font-family: inherit;
  }
  .editor .ti { font-size: 1.4rem; padding: 1.5rem 1.5rem .5rem; }
  .editor textarea { font-size: .9rem; padding: .5rem 1.5rem 1.5rem;
                     line-height: 1.6; resize: none; }
  .editor .bar {
    border-top: 1px solid #2a2a2a; padding: .7rem 1.5rem; display: flex;
    justify-content: space-between; align-items: center; font-size: .75rem; color: #555;
    letter-spacing: .1em;
  }
  .editor .bar button.del {
    cursor: pointer; border: 1px solid #333; background: transparent; color: #888;
    padding: .35rem .9rem; border-radius: 3px; font-family: inherit; font-size: .7rem;
    letter-spacing: .15em;
  }
  .editor .bar button.del:hover { color: crimson; border-color: crimson; }
  .editor.empty { display: flex; align-items: center; justify-content: center;
                  color: #555; font-size: .8rem; letter-spacing: .15em; }
CSS

# ---- rendering ------------------------------------------------------------

def html_escape(s)
  s.to_s.gsub("&", "&amp;").gsub("<", "&lt;").gsub(">", "&gt;").gsub('"', "&quot;")
end

def render_list(notes, active_id = nil, q = "")
  if notes.empty?
    inner = q.empty? ? "<div class='empty-list'>NO NOTES</div>"
                     : "<div class='empty-list'>NO MATCHES</div>"
    return "<aside class='list' id='list'>#{inner}</aside>"
  end
  items = notes.map do |n|
    cls = n[:id] == active_id ? "item active" : "item"
    title = n[:title].empty? ? "(untitled)" : n[:title]
    preview = n[:body].to_s.gsub("\n", " ")[0, 80]
    "<div class='#{cls}' rb-get='/note/#{n[:id]}' rb-target='#main' rb-swap='outerHTML'>" \
      "<div class='t'>#{html_escape(title)}</div>" \
      "<div class='p'>#{html_escape(preview)}</div>" \
    "</div>"
  end.join
  "<aside class='list' id='list'>#{items}</aside>"
end

def render_editor(note)
  return "<section class='editor empty' id='editor'>— SELECT OR CREATE A NOTE —</section>" unless note
  ts = (Time.at(note[:updated_at]).strftime("%Y-%m-%d %H:%M") rescue note[:updated_at].to_s)
  <<~HTML
    <section class="editor" id="editor">
      <input class="ti" name="title" value="#{html_escape(note[:title])}"
             placeholder="untitled"
             rb-put="/note/#{note[:id]}"
             rb-vals='{"id":"#{note[:id]}"}'
             rb-trigger="input changed delay:300ms"
             rb-target="#list" rb-swap="outerHTML">
      <textarea name="body" placeholder="start writing..."
                rb-put="/note/#{note[:id]}"
                rb-vals='{"id":"#{note[:id]}"}'
                rb-trigger="input changed delay:400ms"
                rb-target="#list" rb-swap="outerHTML">#{html_escape(note[:body])}</textarea>
      <div class="bar">
        <span>SAVED · #{ts}</span>
        <button class="del"
                rb-delete="/note/#{note[:id]}"
                rb-target="#main" rb-swap="outerHTML">DELETE</button>
      </div>
    </section>
  HTML
end

def render_main(active_id: nil, q: "")
  notes = filter_notes(q)
  active = active_id ? $store&.get(active_id) : notes.first
  list = render_list(notes, active && active[:id], q)
  editor = render_editor(active)
  "<div class='layout' id='main'>#{list}#{editor}</div>"
end

def filter_notes(q)
  return [] unless $store
  notes = $store.all
  return notes if q.to_s.strip.empty?
  qq = q.downcase
  notes.select { |n| n[:title].downcase.include?(qq) || n[:body].downcase.include?(qq) }
end

def render_page
  banner = HAS_DEPS ? "" :
    "<div style='background:crimson;color:#000;padding:.6rem 1.5rem;font-size:.8rem;" \
    "letter-spacing:.15em'>mruby-lmdb / mruby-cbor not loaded — running in read-only demo mode</div>"
  <<~HTML
    <!doctype html><html><head><meta charset="utf-8"><title>notes</title>
    #{MRUBY_ROUTER_EXT}<style>#{CSS}</style></head>
    <body>
    #{banner}
    <header>
      <h1>NOTES x MRUBY</h1>
      <input class="search" type="text" name="q" placeholder="search..."
             autocomplete="off"
             rb-get="/search"
             rb-trigger="input changed delay:200ms"
             rb-target="#main" rb-swap="outerHTML">
      <button rb-post="/note" rb-target="#main" rb-swap="outerHTML">+ NEW</button>
    </header>
    #{render_main}
    </body></html>
  HTML
end

# ---- routing --------------------------------------------------------------

def note_id_from(path)
  prefix = "/note/"
  return nil unless path.start_with?(prefix)
  rest = path[prefix.length..]
  rest.empty? ? nil : rest
end

def route(method, path, params)
  return "<div class='editor empty' id='editor'>storage unavailable</div>" unless $store

  case method
  when "GET"
    if path == "/search"
      render_main(q: params["q"].to_s)
    elsif (id = note_id_from(path))
      render_main(active_id: id)
    end
  when "POST"
    if path == "/note"
      id = $store.new_id
      $store.put(id, title: "", body: "")
      render_main(active_id: id)
    end
  when "PUT"
    if (id = note_id_from(path))
      existing = $store.get(id) || { title: "", body: "" }
      $store.put(id,
                 title: params["title"] || existing[:title],
                 body:  params["body"]  || existing[:body])
      # only re-render the list — editor is the source of truth for the field the user is in
      render_list($store.all, id)
    end
  when "DELETE"
    if (id = note_id_from(path))
      $store.delete(id)
      render_main
    end
  end || "<p style='color:crimson'>404 #{method} #{path}</p>"
end

Webview.open(title: "notes x mruby", size: [900, 620], debug: true) do |w|
  w.bind(:htmx_route) { |m, p, params| route(m, p, params) }
  w.html = render_page
end
