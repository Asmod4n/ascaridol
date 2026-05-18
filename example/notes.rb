# Persistent note-taking app for Hypha, using mruby-lmdb + mruby-cbor.
#
# Build:
#   conf.gem '<path-to-hypha-mrb>' do |hypha|
#     hypha.hypha_main = File.expand_path('example/notes.rb', __dir__)
#   end
#   rake
#
# Run:
#   mruby/build/host/bin/hypha
#
# Dependencies are pulled in transitively by hypha-mrb (mruby-lmdb,
# mruby-cbor, mruby-mustache), so no extra `conf.gem` lines are needed.
#
# Demonstrates:
#   - A real, persistent app: notes survive restarts
#   - LMDB for storage, CBOR for the on-disk note format
#   - Mustache templates compiled once at load, all rendering goes through them
#   - Debounced search-as-you-type via rb-trigger="input changed delay:200ms"
#   - Master/detail layout where edits round-trip through Ruby
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
    notes.sort { |a, b| b[:updated_at] <=> a[:updated_at] }
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
    @counter ||= 0
    @counter += 1
    "n_#{Time.now.to_i}_#{@counter}"
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

# ---- styles ---------------------------------------------------------------

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
  .dep-banner { background: crimson; color: #000; padding: .6rem 1.5rem;
                font-size: .8rem; letter-spacing: .15em; }
  .layout { display: grid; grid-template-columns: 280px 1fr; min-height: 0; }
  .list { border-right: 1px solid #2a2a2a; overflow-y: auto; }
  .item { padding: .85rem 1.2rem; border-bottom: 1px solid #1c1c1c; cursor: pointer; }
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

# ---- mustache templates ---------------------------------------------------
#
# All HTML output goes through these. Templates are compiled once at load.
# Mustache `{{var}}` HTML-escapes by default; use `{{{var}}}` for trusted
# pre-rendered HTML fragments (CSS, router script, nested template output).

SOURCES = {
  'page' => <<~MUSTACHE,
    <!doctype html><html><head><meta charset="utf-8"><title>notes</title>
    #{Hypha.html_router(:route)}<style>#{CSS}</style></head>
    <body>
    {{^has_deps}}<div class="dep-banner">mruby-lmdb / mruby-cbor not loaded — running in read-only demo mode</div>{{/has_deps}}
    <header>
      <h1>NOTES x MRUBY</h1>
      <input class="search" type="text" name="q" placeholder="search..."
             autocomplete="off"
             rb-get="/search"
             rb-trigger="input changed delay:200ms"
             rb-target="#main" rb-swap="outerHTML">
      <button rb-post="/note" rb-target="#main" rb-swap="outerHTML">+ NEW</button>
    </header>
    {{{main}}}
    </body></html>
  MUSTACHE

  'main' => <<~'MUSTACHE',
    <div class="layout" id="main">{{{list}}}{{{editor}}}</div>
  MUSTACHE

  'list' => <<~'MUSTACHE',
    <aside class="list" id="list">{{#has_items}}{{#items}}{{>item}}{{/items}}{{/has_items}}{{^has_items}}<div class="empty-list">{{empty_msg}}</div>{{/has_items}}</aside>
  MUSTACHE

  'item' => <<~'MUSTACHE',
    <div class="{{item_class}}" rb-get="/note/{{id}}" rb-target="#main" rb-swap="outerHTML"><div class="t">{{display_title}}</div><div class="p">{{preview}}</div></div>
  MUSTACHE

  'editor' => <<~'MUSTACHE',
    <section class="editor" id="editor">
      <input class="ti" name="title" value="{{title}}"
             placeholder="untitled"
             rb-put="/note/{{id}}"
             rb-vals='{"id":"{{id}}"}'
             rb-trigger="input changed delay:300ms"
             rb-target="#list" rb-swap="outerHTML">
      <textarea name="body" placeholder="start writing..."
                rb-put="/note/{{id}}"
                rb-vals='{"id":"{{id}}"}'
                rb-trigger="input changed delay:400ms"
                rb-target="#list" rb-swap="outerHTML">{{body}}</textarea>
      <div class="bar">
        <span>SAVED · {{display_ts}}</span>
        <button class="del"
                rb-delete="/note/{{id}}"
                rb-target="#main" rb-swap="outerHTML">DELETE</button>
      </div>
    </section>
  MUSTACHE

  'editor_empty' => %(<section class="editor empty" id="editor">— SELECT OR CREATE A NOTE —</section>),

  'storage_unavailable' => %(<div class="editor empty" id="editor">storage unavailable</div>),

  'not_found' => %(<p style="color:crimson">404 {{method}} {{path}}</p>),
}

TPL = SOURCES.transform_values { |src| Mustache::Template.compile(src) }

# Render any template by name. Passing TPL as the partials hash lets templates
# reference each other via {{>name}} — e.g. 'list' includes 'item'.
def render(name, data = nil) = TPL.fetch(name).render(data, TPL)

# ---- view-data prep -------------------------------------------------------

def item_view(n, active_id)
  {
    id:            n[:id],
    item_class:    n[:id] == active_id ? 'item active' : 'item',
    display_title: n[:title].empty? ? '(untitled)' : n[:title],
    preview:       n[:body].to_s.gsub("\n", " ")[0, 80],
  }
end

def list_view(notes, active_id, q)
  {
    has_items: !notes.empty?,
    items:     notes.map { |n| item_view(n, active_id) },
    empty_msg: q.to_s.strip.empty? ? 'NO NOTES' : 'NO MATCHES',
  }
end

def editor_view(note)
  return nil unless note
  ts = (Time.at(note[:updated_at]).strftime("%Y-%m-%d %H:%M") rescue note[:updated_at].to_s)
  {
    id:         note[:id],
    title:      note[:title],
    body:       note[:body],
    display_ts: ts,
  }
end

# ---- rendering ------------------------------------------------------------

def render_list(notes, active_id = nil, q = "")
  render('list', list_view(notes, active_id, q))
end

def render_editor(note)
  v = editor_view(note)
  v ? render('editor', v) : render('editor_empty')
end

def render_main(active_id: nil, q: "")
  notes  = filter_notes(q)
  active = active_id ? $store&.get(active_id) : notes.first
  render('main',
         list:   render_list(notes, active && active[:id], q),
         editor: render_editor(active))
end

def filter_notes(q)
  return [] unless $store
  notes = $store.all
  return notes if q.to_s.strip.empty?
  qq = q.downcase
  notes.select { |n| n[:title].downcase.include?(qq) || n[:body].downcase.include?(qq) }
end

def render_page
  render('page',
         has_deps: HAS_DEPS,
         main:     render_main)
end

# ---- routing --------------------------------------------------------------

def note_id_from(path)
  prefix = "/note/"
  return nil unless path.start_with?(prefix)
  rest = path[prefix.length..]
  rest.empty? ? nil : rest
end

def route(method, path, params)
  return render('storage_unavailable') unless $store

  result =
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
    end

  result || render('not_found', method: method, path: path)
end

Hypha.run(title: "notes x mruby", size: [900, 620]) do |w|
  w.bind(:route) do |m, p, params|
    route(m, p, params || {})
  end
  w.html = render_page
end