# Tabs + modal dialogs in mruby-webview, htmx-style.
#
# Run with:  ./bin/mruby examples/tabs_and_modals/app.rb
#
# Demonstrates:
#   - Tab strip where each tab swaps a content panel
#   - "Active" tab tracked entirely on the server
#   - Modal dialog opened by appending fragments to <body>
#   - Modal dismissal via rb-swap="delete" (custom router op)
#   - Inline detail rows that expand/collapse with afterend / delete
#
# All UI is a pure HTML fragment returned from Ruby. The router only
# needs the standard verb attributes plus a couple of extra swap modes.

PROJECTS = [
  { id: 1, name: "atlas",   status: "shipping",   owner: "alice", desc: "Cross-platform GUI runtime built on system webviews. Now in beta." },
  { id: 2, name: "beacon",  status: "in-review",  owner: "bob",   desc: "Distributed log aggregation for embedded fleets." },
  { id: 3, name: "cipher",  status: "drafting",   owner: "carol", desc: "Lightweight key rotation service with per-tenant scopes." },
  { id: 4, name: "dynamo",  status: "shipping",   owner: "dan",   desc: "Background job queue with priority lanes." },
  { id: 5, name: "ember",   status: "blocked",    owner: "eve",   desc: "Hot-reloading config service. Awaiting security review." },
]

PEOPLE = [
  { name: "alice",  role: "lead",      tz: "UTC+1" },
  { name: "bob",    role: "engineer",  tz: "UTC-5" },
  { name: "carol",  role: "engineer",  tz: "UTC+9" },
  { name: "dan",    role: "designer",  tz: "UTC+0" },
  { name: "eve",    role: "engineer",  tz: "UTC-3" },
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

  /* modal */
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
CSS

def render_tab_strip(active)
  tabs = [[:projects, "PROJECTS"], [:people, "PEOPLE"], [:about, "ABOUT"]]
  inner = tabs.map do |id, label|
    cls = active == id ? "tab active" : "tab"
    "<button class='#{cls}' rb-get='/tab/#{id}' rb-target='#main' rb-swap='outerHTML'>#{label}</button>"
  end.join
  "<nav class='tabs'>#{inner}</nav>"
end

def render_main(active)
  body = case active
         when :projects then render_projects
         when :people   then render_people
         else                render_about
         end
  "<main id='main'>#{render_tab_strip(active)}<div class='panel'>#{body}</div></main>"
end

def render_projects
  rows = PROJECTS.map { |p| render_row(p) }.join
  <<~HTML
    <table>
      <thead><tr><th>Name</th><th>Status</th><th>Owner</th><th></th></tr></thead>
      #{rows}
    </table>
    <p style="color:#555; font-size: .75rem; margin-top: 1.2rem; letter-spacing: .1em;">
      CLICK A ROW TO TOGGLE DETAILS · ESC TO DISMISS MODALS
    </p>
  HTML
end

def render_row(p)
  "<tbody class='proj' id='row-#{p[:id]}'>" \
    "<tr class='row' rb-get='/project/#{p[:id]}/toggle' " \
        "rb-target='#row-#{p[:id]}' rb-swap='outerHTML'>" \
      "<td>#{p[:name]}</td>" \
      "<td><span class='pill #{p[:status]}'>#{p[:status]}</span></td>" \
      "<td>#{p[:owner]}</td>" \
      "<td class='actions'>" \
        "<button rb-get='/project/#{p[:id]}/delete-confirm' " \
                "rb-target='body' rb-swap='beforeend' " \
                "onclick='event.stopPropagation()'>DELETE</button>" \
      "</td>" \
    "</tr>" \
  "</tbody>"
end

def render_row_expanded(p)
  "<tbody class='proj' id='row-#{p[:id]}'>" \
    "<tr class='row expanded' rb-get='/project/#{p[:id]}/collapse' " \
        "rb-target='#row-#{p[:id]}' rb-swap='outerHTML'>" \
      "<td>#{p[:name]}</td>" \
      "<td><span class='pill #{p[:status]}'>#{p[:status]}</span></td>" \
      "<td>#{p[:owner]}</td>" \
      "<td class='actions'>" \
        "<button rb-get='/project/#{p[:id]}/delete-confirm' " \
                "rb-target='body' rb-swap='beforeend' " \
                "onclick='event.stopPropagation()'>DELETE</button>" \
      "</td>" \
    "</tr>" \
    "<tr class='detail' rb-get='/project/#{p[:id]}/collapse' " \
        "rb-target='#row-#{p[:id]}' rb-swap='outerHTML'>" \
      "<td colspan='4'>" \
        "<strong style='color:#c8f542'>#{p[:name]}</strong> &mdash; " \
        "owned by #{p[:owner]} · status #{p[:status]}<br>" \
        "<span style='color:#666'>#{p[:desc]}</span>" \
        "<div style='color:#444; font-size:.7rem; margin-top:.6rem; letter-spacing:.15em'>CLICK TO COLLAPSE</div>" \
      "</td>" \
    "</tr>" \
  "</tbody>"
end

def render_people
  rows = PEOPLE.map do |p|
    "<tr><td>#{p[:name]}</td><td>#{p[:role]}</td><td>#{p[:tz]}</td></tr>"
  end.join
  "<table><thead><tr><th>Name</th><th>Role</th><th>TZ</th></tr></thead><tbody>#{rows}</tbody></table>"
end

def render_about
  <<~HTML
    <div style="max-width: 480px; line-height: 1.6; color: #aaa; font-size: .9rem;">
      <p>Tabs and modals composed entirely from server-rendered HTML fragments.</p>
      <p style="margin-top: .8rem;">No client state, no JSON wire format &mdash; just
      Ruby returning HTML for every interaction. The custom router supplies
      <code>afterend</code>, <code>beforeend</code>, and a synthetic
      <code>delete</code> swap.</p>
    </div>
  HTML
end

def render_delete_modal(p)
  <<~HTML
    <div id="modal" class="scrim"
         rb-get="" rb-trigger="click" rb-target="#modal" rb-swap="delete">
      <div class="modal" onclick="event.stopPropagation()">
        <h3>DELETE PROJECT</h3>
        <p>Permanently delete <strong style="color:#c8f542">#{p[:name]}</strong>?
        This cannot be undone.</p>
        <div class="row-buttons">
          <button onclick="document.getElementById('modal').remove()">CANCEL</button>
          <button class="danger"
                  rb-delete="/project/#{p[:id]}"
                  rb-target="#main"
                  rb-swap="outerHTML"
                  onclick="setTimeout(function(){var m=document.getElementById('modal');if(m)m.remove();},0)">DELETE</button>
        </div>
      </div>
    </div>
  HTML
end

def render_page
  <<~HTML
    <!doctype html><html><head><meta charset="utf-8"><title>tabs + modals</title>
    #{Hypha.html_router(:route)}<style>#{CSS}</style></head>
    <body><h1>TABS + MODALS x MRUBY</h1>
    #{render_main(:projects)}
    </body></html>
  HTML
end

def find_project(id) = PROJECTS.find { |p| p[:id] == id.to_i }
def delete_project(id)  = PROJECTS.reject! { |p| p[:id] == id.to_i }

# Tiny path matcher — avoids mruby's missing Regexp. Returns the captured
# trailing segment if `path` starts with `prefix` and has more after it,
# otherwise nil.
def path_after(path, prefix)
  return nil unless path.start_with?(prefix)
  rest = path[prefix.length..]
  rest.empty? ? nil : rest
end

# Extract the leading numeric segment from a string like "1/toggle" -> "1".
def leading_id(s)
  i = 0
  i += 1 while i < s.length && s[i] >= "0" && s[i] <= "9"
  s[0, i]
end

def route(method, path, _params)
  case method
  when "GET"
    if (rest = path_after(path, "/tab/"))
      render_main(rest.to_sym)
    elsif (rest = path_after(path, "/project/"))
      id = leading_id(rest)
      if rest.end_with?("/delete-confirm")
        render_delete_modal(find_project(id))
      elsif rest.end_with?("/toggle")
        render_row_expanded(find_project(id))
      elsif rest.end_with?("/collapse")
        render_row(find_project(id))
      end
    end
  when "DELETE"
    if (rest = path_after(path, "/project/"))
      delete_project(rest)
      # Replace the whole main view so list, modal, and any open detail rows
      # all reset cleanly. The router target for the DELETE button is #modal,
      # so we need to dismiss it client-side too.
      render_main(:projects)
    end
  end || "<p style='color:crimson'>404 #{method} #{path}</p>"
end

Hypha.run(title: "tabs + modals x mruby", size: [820, 620], debug: true) do |w|
  w.bind(:route) { |m, p, params| route(m, p, params) }
  w.html = render_page
end
