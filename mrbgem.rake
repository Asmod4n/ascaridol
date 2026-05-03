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

  # ------------------------------------------------------------------------
  # webview source: shipped as a git submodule under vendor/webview
  # ------------------------------------------------------------------------
  webview_dir = ENV['MRUBY_WEBVIEW_DIR'] || File.join(spec.dir, 'vendor', 'webview')
  webview_inc = File.join(webview_dir, 'core', 'include')

  unless File.directory?(webview_inc)
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

  spec.cc.include_paths  << webview_inc
  spec.cxx.include_paths << webview_inc

  # ------------------------------------------------------------------------
  # Detect the *target* platform we're building mruby for. mrbgem.rake runs
  # under CRuby, so RUBY_PLATFORM would describe the host (and break cross
  # compilation). Inspect the build's toolchain, the target triple of
  # MRuby::CrossBuild and the cc command name instead.
  # ------------------------------------------------------------------------
  build       = spec.build
  toolchains  = Array(build.respond_to?(:toolchains) ? build.toolchains : [])
  cc_command  = build.cc.command.to_s
  host_target = build.respond_to?(:host_target) ? build.host_target.to_s : ''

  is_windows = toolchains.include?('visualcpp') ||
               host_target =~ /mingw|cygwin|msys|win32|windows/i ||
               cc_command  =~ /(?:^|[\/\\-])(?:mingw|w64|cl)(?:\.exe|$|[-.])/i
  is_darwin  = host_target =~ /darwin|apple|mac/i ||
               cc_command  =~ /apple|darwin/i ||
               (host_target.empty? && !is_windows && `uname -s 2>/dev/null`.strip == 'Darwin')

  # Extra flags we need to compile webview.cc itself (not just the .c bindings).
  webview_extra_cflags = []

  if is_windows
    spec.linker.libraries.concat(%w[advapi32 ole32 shell32 shlwapi user32 version])
    spec.cxx.flags << '-DWEBVIEW_EDGE'
    webview_extra_cflags << '-DWEBVIEW_EDGE'
  elsif is_darwin
    spec.linker.flags_after_libraries.concat(%w[-framework WebKit -framework Cocoa])
    spec.linker.libraries << 'c++'
  else
    # Linux / *BSD — discover GTK + WebKitGTK via pkg-config.
    pkg = ENV['MRUBY_WEBVIEW_PKG']
    pkg ||= %w[gtk4 webkitgtk-6.0].all? { |p| system("pkg-config --exists #{p}") } ? 'gtk4 webkitgtk-6.0' : nil
    pkg ||= %w[gtk+-3.0 webkit2gtk-4.1].all? { |p| system("pkg-config --exists #{p}") } ? 'gtk+-3.0 webkit2gtk-4.1' : nil
    pkg ||= %w[gtk+-3.0 webkit2gtk-4.0].all? { |p| system("pkg-config --exists #{p}") } ? 'gtk+-3.0 webkit2gtk-4.0' : nil

    abort <<~MSG unless pkg
      [mruby-webview] no GTK + WebKitGTK development packages found via pkg-config.
      Install one of:
        - gtk4 + webkitgtk-6.0   (Debian/Ubuntu: libgtk-4-dev libwebkitgtk-6.0-dev)
        - gtk+-3.0 + webkit2gtk-4.1   (libgtk-3-dev libwebkit2gtk-4.1-dev)
        - gtk+-3.0 + webkit2gtk-4.0   (libgtk-3-dev libwebkit2gtk-4.0-dev)
      Or set MRUBY_WEBVIEW_PKG to a custom pkg-config package list.
    MSG

    cflags = `pkg-config --cflags #{pkg}`.strip.split(/\s+/).reject(&:empty?)
    libs   = `pkg-config --libs   #{pkg}`.strip.split(/\s+/).reject(&:empty?)

    # Split pkg-config --cflags so each kind of flag goes to the right channel.
    cflags.each do |f|
      case f
      when /\A-I(.+)/  then
        spec.cc.include_paths  << $1
        spec.cxx.include_paths << $1
      when /\A-D(.+)/  then
        spec.cc.defines  << $1
        spec.cxx.defines << $1
      else
        spec.cc.flags  << f
        spec.cxx.flags << f
      end
    end

    spec.linker.flags_after_libraries.concat(libs)
    spec.linker.libraries << 'stdc++'
    spec.linker.libraries << 'pthread'

    # Mirror the same flags into our webview.cc rule below.
    webview_extra_cflags.concat(cflags)
  end

  # ------------------------------------------------------------------------
  # Compile vendor/webview/core/src/webview.cc with the build's C++ compiler.
  # We can't drop it into src/ because mruby would treat it as a gem source
  # (and our naming/extension expectations differ), so we build it explicitly
  # and add the resulting object to spec.objs.
  # ------------------------------------------------------------------------
  webview_src = File.join(webview_dir, 'core', 'src', 'webview.cc')
  obj_ext     = build.exts.object
  webview_obj = File.join(spec.build_dir, 'vendor', 'webview', "webview#{obj_ext}")

  directory File.dirname(webview_obj)
  file webview_obj => [webview_src, File.dirname(webview_obj)] do |t|
    cxx = build.cxx
    flag_parts = []
    flag_parts.concat(Array(cxx.flags).flatten)
    flag_parts.concat(Array(cxx.defines).map { |d| "-D#{d}" })
    flag_parts.concat(Array(cxx.include_paths).map { |p| "-I#{p}" })
    flag_parts.concat(Array(spec.cxx.flags).flatten)
    flag_parts.concat(Array(spec.cxx.defines).map { |d| "-D#{d}" })
    flag_parts.concat(Array(spec.cxx.include_paths).map { |p| "-I#{p}" })
    flag_parts.concat(webview_extra_cflags)
    flag_parts << '-std=c++14' unless flag_parts.any? { |f| f =~ /-std=c\+\+/ }
    flag_parts << '-fPIC'      unless is_windows || flag_parts.include?('-fPIC')
    sh "#{cxx.command} #{flag_parts.uniq.join(' ')} -c #{t.prerequisites.first} -o #{t.name}"
  end
  spec.objs << webview_obj
end
