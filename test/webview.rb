# mruby-webview tests.
#
# Many tests skip rendering a real window (which would need a display server)
# and instead verify that the API surface, error hierarchy and defaults look
# correct. Tests that need a webview instance are guarded by DISPLAY/DEBUG_TEST.

assert('Webview is defined') do
  assert_kind_of Class, Webview
end

assert('Webview error hierarchy') do
  assert_kind_of Class, Webview::Error
  %w[MissingDependencyError CanceledError InvalidStateError
     InvalidArgumentError DuplicateError NotFoundError DestroyedError].each do |n|
    cls = Webview.const_get(n)
    assert_kind_of Class, cls
    assert_true cls < Webview::Error
  end
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

# Live tests — only run if DISPLAY is set. Skipping cleanly when not.
if ENV['DISPLAY'] || ENV['MRUBY_WEBVIEW_LIVE']
  assert('Webview lifecycle: create then destroy') do
    w = Webview.new(debug: false)
    assert_false w.destroyed?
    w.destroy
    assert_true w.destroyed?
  end

  assert('Webview accepts title and size in initializer') do
    w = Webview.new(title: 'Test', size: [320, 240, :fixed])
    assert_false w.destroyed?
    w.destroy
  end

  assert('Webview raises DestroyedError after destroy') do
    w = Webview.new
    w.destroy
    assert_raise(Webview::DestroyedError) { w.title = 'nope' }
  end

  assert('bind registers names') do
    w = Webview.new
    w.bind('echo') { |x| x }
    assert_include w.bindings, 'echo'
    w.unbind('echo')
    assert_not_include w.bindings, 'echo'
    w.destroy
  end
end
