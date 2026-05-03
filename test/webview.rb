
assert('Webview.version') do
  v = Webview.version
  assert_kind_of Hash, v
  assert_kind_of Integer, v[:major]
  assert_kind_of Integer, v[:minor]
  assert_kind_of Integer, v[:patch]
  assert_kind_of String,  v[:version]
end
