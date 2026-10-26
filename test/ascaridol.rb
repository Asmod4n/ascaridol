
assert('Webview.version') do
  v = Ascaridol.version
  assert_kind_of Hash, v
  assert_kind_of Integer, v[:major]
  assert_kind_of Integer, v[:minor]
  assert_kind_of Integer, v[:patch]
  assert_kind_of String,  v[:version]
end

# htmx.config.prefix is the second attribute prefix of htmx 4. The page
# reads it from the htmx-config meta tag, so the tag must carry the value
# that the caller gave, and a value that could leave the JSON or the
# attribute must not reach the page.
assert('Ascaridol.htmx sets an empty prefix by default') do
  assert_true Ascaridol.htmx(:route).include?(%q("prefix":""))
end

assert('Ascaridol.htmx passes prefix: to the htmx config') do
  assert_true Ascaridol.htmx(:route, prefix: "data-hx-").include?(%q("prefix":"data-hx-"))
end

assert('Ascaridol.htmx refuses a prefix outside a-z, 0-9 and -') do
  ["'", '"', "<", " ", "DATA-", "x_"].each do |bad|
    assert_raise(ArgumentError) { Ascaridol.htmx(:route, prefix: bad) }
  end
  assert_raise(ArgumentError) { Ascaridol.htmx(:route, prefix: nil) }
end
