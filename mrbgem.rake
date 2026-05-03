MRuby::Gem::Specification.new('mruby-webview') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Hendrik'
  spec.summary = 'Idiomatic Ruby bindings for the webview/webview library'
  spec.version = '0.1.0'

  spec.add_dependency 'mruby-fast-json', github: 'asmod4n/mruby-fast-json'
  spec.add_dependency 'mruby-string-ext', core: 'mruby-string-ext'
  spec.add_dependency 'mruby-hash-ext',   core: 'mruby-hash-ext'
  spec.add_dependency 'mruby-symbol-ext', core: 'mruby-symbol-ext'
  spec.add_dependency 'mruby-error',      core: 'mruby-error'
  spec.add_dependency 'mruby-proc-ext',   core: 'mruby-proc-ext'
  spec.add_dependency 'mruby-metaprog',   core: 'mruby-metaprog'

  # mruby-fast-json requires UTF-8 string support.
  spec.cc.defines  << 'MRB_UTF8_STRING'
  spec.cxx.defines << 'MRB_UTF8_STRING'

  # webview source ships as a git submodule under vendor/webview.
  webview_dir = ENV['MRUBY_WEBVIEW_DIR'] || File.join(spec.dir, 'vendor', 'webview')
  webview_inc = File.join(webview_dir, 'core', 'include')

  unless File.directory?(webview_inc)
    # Try to initialize the submodule automatically (works for sources that
    # were checked out without --recurse-submodules).
    if File.exist?(File.join(spec.dir, '.gitmodules'))
      Dir.chdir(spec.dir) do
        sh 'git submodule update --init --recursive vendor/webview'
      end
    end
  end

  unless File.directory?(webview_inc)
    abort <<~MSG
      [mruby-webview] webview source not found at #{webview_dir}.
      Run `git submodule update --init --recursive` in the gem directory,
      or set MRUBY_WEBVIEW_DIR to point at an existing webview checkout.
    MSG
  end

  spec.cc.include_paths  << File.join(webview_dir, 'core', 'include')
  spec.cxx.include_paths << File.join(webview_dir, 'core', 'include')

  webview_src = File.join(webview_dir, 'core', 'src', 'webview.cc')
  obj_ext     = spec.build.exts.object
  webview_obj = File.join(spec.build_dir, 'vendor', 'webview', "webview#{obj_ext}")

  directory File.dirname(webview_obj)
  file webview_obj => [webview_src, File.dirname(webview_obj)] do |t|
    cxx = spec.build.cxx
    flags = (cxx.flags.flatten + ['-std=c++14', '-fPIC']).join(' ')
    includes = (spec.cxx.include_paths + cxx.include_paths).uniq.map { |p| "-I#{p}" }.join(' ')
    sh "#{cxx.command} #{flags} #{includes} -c #{t.prerequisites.first} -o #{t.name}"
  end
  spec.objs << webview_obj

  case RUBY_PLATFORM
  when /linux|bsd/
    pkg = ENV['MRUBY_WEBVIEW_PKG']
    pkg ||= %w[gtk4 webkitgtk-6.0].all? { |p| system("pkg-config --exists #{p}") } ? 'gtk4 webkitgtk-6.0' : nil
    pkg ||= %w[gtk+-3.0 webkit2gtk-4.1].all? { |p| system("pkg-config --exists #{p}") } ? 'gtk+-3.0 webkit2gtk-4.1' : nil
    pkg ||= %w[gtk+-3.0 webkit2gtk-4.0].all? { |p| system("pkg-config --exists #{p}") } ? 'gtk+-3.0 webkit2gtk-4.0' : nil

    if pkg
      cflags = `pkg-config --cflags #{pkg}`.strip.split(/\s+/)
      libs   = `pkg-config --libs   #{pkg}`.strip.split(/\s+/)
      spec.cc.flags.concat(cflags)
      spec.cxx.flags.concat(cflags)
      spec.linker.flags_after_libraries.concat(libs)
    else
      warn "[mruby-webview] pkg-config not found for gtk/webkit; set MRUBY_WEBVIEW_PKG, e.g. 'gtk4 webkitgtk-6.0'"
    end
    spec.linker.libraries << 'stdc++'
    spec.linker.libraries << 'pthread'
  when /darwin/
    spec.linker.flags_after_libraries.concat(%w[-framework WebKit -framework Cocoa])
    spec.linker.libraries << 'c++'
  when /mingw|mswin|cygwin/
    spec.linker.libraries.concat(%w[advapi32 ole32 shell32 shlwapi user32 version])
    spec.cxx.flags << '-DWEBVIEW_EDGE'
  end
end
