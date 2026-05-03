# mruby-webview tests.
#
# Many tests skip rendering a real window (which would need a display server)
# and instead verify that the API surface, error hierarchy and defaults look
# correct. Tests that need a webview instance are guarded by DISPLAY/DEBUG_TEST.

assert('Webview is defined') do
  assert_kind_of Class, Webview
end



assert('Webview hint constants') do
  assert_equal 0, Webview::HINT_NONE
  assert_equal 1, Webview::HINT_MIN
  assert_equal 2, Webview::HINT_MAX
  assert_equal 3, Webview::HINT_FIXED
end

assert('Webview hint symbols') do
  assert_equal Webview::HINT_NONE,  Webview::HINTS[:none]
  assert_equal Webview::HINT_FIXED, Webview::HINTS[:fixed]
end

assert('Webview.version') do
  v = Webview.version
  assert_kind_of Hash, v
  assert_kind_of Integer, v[:major]
  assert_kind_of Integer, v[:minor]
  assert_kind_of Integer, v[:patch]
  assert_kind_of String,  v[:version]
end
