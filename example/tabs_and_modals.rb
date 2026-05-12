# Tabs + modal dialogs in hypha-mrb, htmx-style, with mruby-mustache.
#
# Same behavior as the string-concat version. Presentation lives in
# pre-compiled Mustache templates; Ruby only prepares view data.

PROJECTS = [
  { id: 1, name: 'atlas',  status: 'shipping',  owner: 'alice', desc: 'Cross-platform GUI runtime built on system webviews. Now in beta.' },
  { id: 2, name: 'beacon', status: 'in-review', owner: 'bob',   desc: 'Distributed log aggregation for embedded fleets.' },
  { id: 3, name: 'cipher', status: 'drafting',  owner: 'carol', desc: 'Lightweight key rotation service with per-tenant scopes.' },
  { id: 4, name: 'dynamo', status: 'shipping',  owner: 'dan',   desc: 'Background job queue with priority lanes.' },
  { id: 5, name: 'ember',  status: 'blocked',   owner: 'eve',   desc: 'Hot-reloading config service. Awaiting security review.' },
]

PEOPLE = [
  { name: 'alice', role: 'lead',     tz: 'UTC+1' },
  { name: 'bob',   role: 'engineer', tz: 'UTC-5' },
  { name: 'carol', role: 'engineer', tz: 'UTC+9' },
  { name: 'dan',   role: 'designer', tz: 'UTC+0' },
  { name: 'eve',   role: 'engineer', tz: 'UTC-3' },
]

TABS = [
  { id: 'projects', label: 'PROJECTS' },
  { id: 'people',   label: 'PEOPLE'   },
  { id: 'about',    label: 'ABOUT'    },
]

CSS = <<~'CSS'
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Courier New', monospace; background: #0f0f0f; color: #e8e8e8; padding: 2rem; }
  h1 { font-size: 1.1rem; color: #888; letter-spacing: .2em; margin-bottom: 1.5rem; }

  .tabs { display: flex; gap: 0; border-bottom: 1px solid #2a2a2a; margin-bottom: 1.5rem; }
  .tab {
    background: none; border: none; color: #555; font-family: inherit; font-size: .8rem;
    letter-spacing: .15em; padding: .8rem 1.4rem; cursor: pointer;
    border-bottom: 2px solid transparent;
  }
  .tab:hover { color: #888; }
  .tab.active { color: #c8f542; border-bottom-color: #c8f542; }

  .panel { animation: fade .2s ease-out; }
  @keyframes fade { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; } }

  table { width: 100%; border-collapse: collapse; font-size: .85rem; }
  th, td { text-align: left; padding: .7rem 1rem; border-bottom: 1px solid #222; }
  th { color: #555; font-weight: normal; letter-spacing: .15em; font-size: .7rem; text-transform: uppercase; }
  tr.row { cursor: pointer; }
  tr.row:hover { background: #161616; }
  tr.row.expanded { background: #1a1a1a; }
  tr.row.expanded td:first-child { border-left: 2px solid #c8f542; padding-left: calc(1rem - 2px); }
  tr.detail { background: #141414; cursor: pointer; }
  tr.detail:hover { background: #181818; }
  tr.detail td { padding: 1rem 1.4rem; color: #999; font-size: .8rem; line-height: 1.5; }

  .pill {
    display: inline-block; padding: .15rem .55rem; border-radius: 99px; font-size: .7rem;
    border: 1px solid currentColor; letter-spacing: .1em;
  }
  .pill.shipping  { color: #c8f542; }
  .pill.in-review { color: #d97706; }
  .pill.drafting  { color: #888; }
  .pill.blocked   { color: crimson; }

  .actions button {
    cursor: pointer; border: 1px solid #333; border-radius: 3px; background: transparent;
    color: #888; padding: .25rem .7rem; font-family: inherit; font-size: .7rem; letter-spacing: .1em;
  }
  .actions button:hover { color: #c8f542; border-color: #c8f542; }

  .scrim {
    position: fixed; inset: 0; background: #000a; backdrop-filter: blur(2px);
    display: flex; align-items: center; justify-content: center; padding: 2rem;
    animation: scrim-in .15s ease-out;
  }
  @keyframes scrim-in { from { opacity: 0; } to { opacity: 1; } }
  .modal {
    background: #181818; border: 1px solid #2a2a2a; border-radius: 4px;
    padding: 1.6rem 1.8rem; max-width: 440px; width: 100%;
    box-shadow: 0 12px 40px #000c;
    animation: modal-in .2s cubic-bezier(.2,.7,.2,1);
  }
  @keyframes modal-in {
    from { opacity: 0; transform: translateY(12px) scale(.97); }
    to   { opacity: 1; }
  }
  .modal h3 { font-size: .8rem; color: #888; letter-spacing: .15em; margin-bottom: 1rem; }
  .modal p  { font-size: .85rem; color: #ccc; line-height: 1.5; margin-bottom: 1.2rem; }
  .modal .row-buttons { display: flex; gap: .5rem; justify-content: flex-end; }
  .modal button {
    cursor: pointer; border-radius: 3px; padding: .45rem 1.1rem; font-family: inherit;
    font-size: .8rem; border: 1px solid #333; background: transparent; color: #ccc;
  }
  .modal button.danger { color: crimson; border-color: crimson; }
  .modal button.danger:hover { background: #dc143c20; }
  .modal button:hover { background: #232323; }

  .accent     { color: #c8f542; }
  .muted      { color: #666; }
  .hint       { color: #555; font-size: .75rem; margin-top: 1.2rem; letter-spacing: .1em; }
  .hint-small { color: #444; font-size: .7rem; margin-top: .6rem; letter-spacing: .15em; }
  .about      { max-width: 480px; line-height: 1.6; color: #aaa; font-size: .9rem; }
  .about p + p { margin-top: .8rem; }
  .err        { color: crimson; }
CSS

# ---------------------------------------------------------------------------
# Templates — compiled once, reused forever. TPL doubles as the partials hash.
# ---------------------------------------------------------------------------

SOURCES = {
  'page' => <<~MUSTACHE,
    <!doctype html><html><head><meta charset="utf-8"><title>tabs + modals</title>
    {{{router_script}}}<style>{{{css}}}</style></head>
    <body><h1>TABS + MODALS x MRUBY</h1>
    {{{main}}}
    </body></html>
  MUSTACHE

  'main' => <<~MUSTACHE,
    <main id="main">
      <nav class="tabs">
        {{#tabs}}
        <button class="tab{{#active}} active{{/active}}" rb-get="/tab/{{id}}" rb-target="#main" rb-swap="outerHTML">{{label}}</button>
        {{/tabs}}
      </nav>
      <div class="panel">{{{body}}}</div>
    </main>
  MUSTACHE

  'projects' => <<~MUSTACHE,
    <table>
      <thead><tr><th>Name</th><th>Status</th><th>Owner</th><th></th></tr></thead>
      {{#projects}}{{>project_row}}{{/projects}}
    </table>
    <p class="hint">CLICK A ROW TO TOGGLE DETAILS</p>
  MUSTACHE

  'project_row' => <<~MUSTACHE,
    <tbody class="proj" id="row-{{id}}">
      <tr class="row{{#expanded}} expanded{{/expanded}}" rb-get="{{toggle_url}}" rb-target="#row-{{id}}" rb-swap="outerHTML">
        <td>{{name}}</td>
        <td><span class="pill {{status}}">{{status}}</span></td>
        <td>{{owner}}</td>
        <td class="actions">
          <button rb-get="/project/{{id}}/delete-confirm" rb-target="body" rb-swap="beforeend" onclick="event.stopPropagation()">DELETE</button>
        </td>
      </tr>
      {{#expanded}}
      <tr class="detail" rb-get="/project/{{id}}/collapse" rb-target="#row-{{id}}" rb-swap="outerHTML">
        <td colspan="4">
          <strong class="accent">{{name}}</strong> &mdash; owned by {{owner}} · status {{status}}<br>
          <span class="muted">{{desc}}</span>
          <div class="hint-small">CLICK TO COLLAPSE</div>
        </td>
      </tr>
      {{/expanded}}
    </tbody>
  MUSTACHE

  'people' => <<~MUSTACHE,
    <table>
      <thead><tr><th>Name</th><th>Role</th><th>TZ</th></tr></thead>
      <tbody>
        {{#people}}
        <tr><td>{{name}}</td><td>{{role}}</td><td>{{tz}}</td></tr>
        {{/people}}
      </tbody>
    </table>
  MUSTACHE

  'about' => <<~MUSTACHE,
    <div class="about">
      <p>Tabs and modals composed entirely from server-rendered HTML fragments.</p>
      <p>No client state, no JSON wire format &mdash; just Ruby returning HTML for every interaction. Templates are compiled <code>Mustache::Template</code> objects; the custom router supplies <code>afterend</code>, <code>beforeend</code>, and a synthetic <code>delete</code> swap.</p>
    </div>
  MUSTACHE

  'delete_modal' => <<~MUSTACHE,
    <div id="modal" class="scrim" rb-get="" rb-trigger="click" rb-target="#modal" rb-swap="delete">
      <div class="modal" onclick="event.stopPropagation()">
        <h3>DELETE PROJECT</h3>
        <p>Permanently delete <strong class="accent">{{name}}</strong>? This cannot be undone.</p>
        <div class="row-buttons">
          <button onclick="document.getElementById('modal').remove()">CANCEL</button>
          <button class="danger" rb-delete="/project/{{id}}" rb-target="#main" rb-swap="outerHTML"
                  onclick="setTimeout(function(){var m=document.getElementById('modal');if(m)m.remove();},0)">DELETE</button>
        </div>
      </div>
    </div>
  MUSTACHE

  'not_found' => %(<p class="err">404 {{method}} {{path}}</p>),
}

TPL = SOURCES.transform_values { |src| Mustache::Template.compile(src) }

def render(name, data = nil) = TPL.fetch(name).render(data, TPL)

# ---------------------------------------------------------------------------
# View-data prep
# ---------------------------------------------------------------------------

def tabs_for(active)
  TABS.map { |t| { id: t[:id], label: t[:label], active: t[:id] == active.to_s } }
end

def project_view(p, expanded: false)
  return nil unless p
  p.merge(
    expanded:   expanded,
    toggle_url: expanded ? "/project/#{p[:id]}/collapse" : "/project/#{p[:id]}/toggle",
  )
end

def panel_html(active)
  case active
  when 'projects' then render('projects', projects: PROJECTS.map { |p| project_view(p) })
  when 'people'   then render('people',   people: PEOPLE)
  else                 render('about')
  end
end

def main_html(active)
  render('main', tabs: tabs_for(active), body: panel_html(active))
end

def page_html
  render('page',
         router_script: Hypha.html_router(:route),
         css:           CSS,
         main:          main_html('projects'))
end

# ---------------------------------------------------------------------------
# Routing (unchanged shape — mruby has no Regexp, so leading_id stays)
# ---------------------------------------------------------------------------

def find_project(id)    = PROJECTS.find { |p| p[:id] == id.to_i }
def delete_project(id)  = PROJECTS.reject! { |p| p[:id] == id.to_i }

def path_after(path, prefix)
  return nil unless path.start_with?(prefix)
  rest = path[prefix.length..]
  rest.empty? ? nil : rest
end

def leading_id(s)
  i = 0
  i += 1 while i < s.length && s[i] >= '0' && s[i] <= '9'
  s[0, i]
end

def route(method, path, _params)
  case method
  when 'GET'
    if (rest = path_after(path, '/tab/'))
      main_html(rest)
    elsif (rest = path_after(path, '/project/'))
      id = leading_id(rest)
      p  = find_project(id)
      if    rest.end_with?('/delete-confirm') then p && render('delete_modal', p)
      elsif rest.end_with?('/toggle')         then render('project_row', project_view(p, expanded: true))
      elsif rest.end_with?('/collapse')       then render('project_row', project_view(p))
      end
    end
  when 'DELETE'
    if (rest = path_after(path, '/project/'))
      delete_project(rest)
      main_html('projects')
    end
  end || render('not_found', method: method, path: path)
end

Hypha.run(title: 'tabs + modals x mruby', size: [820, 620], debug: true) do |w|
  w.bind(:route) { |m, p, params| route(m, p, params) }
  w.html = page_html
end
