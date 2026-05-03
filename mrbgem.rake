require 'fileutils'

MRuby::Gem::Specification.new('mruby-webview') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Hendrik'
  spec.summary = 'Idiomatic Ruby bindings for the webview/webview library'
  spec.version = '0.1.0'

  spec.add_dependency 'mruby-fast-json'
  spec.add_dependency 'mruby-string-ext', core: 'mruby-string-ext'
  spec.add_dependency 'mruby-hash-ext',   core: 'mruby-hash-ext'
  spec.add_dependency 'mruby-symbol-ext', core: 'mruby-symbol-ext'
  spec.add_dependency 'mruby-error',      core: 'mruby-error'
  spec.add_dependency 'mruby-proc-ext',   core: 'mruby-proc-ext'
  spec.add_dependency 'mruby-metaprog',   core: 'mruby-metaprog'

  # mruby-fast-json requires UTF-8 string support.
  spec.cc.defines  << 'MRB_UTF8_STRING'
  spec.cxx.defines << 'MRB_UTF8_STRING'

  # We link against webview as a static library, so the bindings TU needs
  # WEBVIEW_STATIC (so the C API decls have plain extern linkage instead of
  # `inline`) and WEBVIEW_HEADER (so the bindings TU only sees declarations,
  # not duplicate definitions of webview_create & friends).
  %w[WEBVIEW_STATIC WEBVIEW_HEADER].each do |d|
    spec.cc.defines  << d
    spec.cxx.defines << d
  end

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
  cxx_command = build.cxx.command.to_s
  host_target = build.respond_to?(:host_target) ? build.host_target.to_s : ''

  is_windows = toolchains.include?('visualcpp') ||
               host_target =~ /mingw|cygwin|msys|win32|windows/i ||
               cc_command  =~ /(?:^|[\/\\-])(?:mingw|w64|cl)(?:\.exe|$|[-.])/i
  is_darwin  = host_target =~ /darwin|apple|mac/i ||
               cc_command  =~ /apple|darwin/i ||
               (host_target.empty? && !is_windows && `uname -s 2>/dev/null`.strip == 'Darwin')

  # System libraries the static webview core depends on at link time.
  if is_windows
    spec.linker.libraries.concat(%w[advapi32 ole32 shell32 shlwapi user32 version])
  elsif is_darwin
    spec.linker.flags_after_libraries.concat(%w[-framework WebKit -framework Cocoa])
    spec.linker.libraries << 'c++'
  else
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

    # Bindings need the GTK headers too (webview.h transitively includes them
    # via the backend headers when WEBVIEW_HEADER isn't set; under
    # WEBVIEW_HEADER they aren't needed, but keeping them lets users that
    # bypass the macro still compile).
    `pkg-config --cflags #{pkg}`.strip.split(/\s+/).reject(&:empty?).each do |f|
      case f
      when /\A-I(.+)/  then spec.cc.include_paths  << $1; spec.cxx.include_paths << $1
      when /\A-D(.+)/  then spec.cc.defines  << $1;       spec.cxx.defines       << $1
      else                  spec.cc.flags    << f;        spec.cxx.flags         << f
      end
    end

    spec.linker.flags_after_libraries.concat(`pkg-config --libs #{pkg}`.strip.split(/\s+/).reject(&:empty?))
    spec.linker.libraries << 'stdc++'
    spec.linker.libraries << 'pthread'
    @gtk_pkg = pkg
  end

  # ------------------------------------------------------------------------
  # Build webview as a static library via its official CMake project.
  # ------------------------------------------------------------------------
  cmake = ENV['CMAKE'] || 'cmake'
  unless system("command -v #{cmake} >/dev/null 2>&1")
    abort "[mruby-webview] cmake not found (looked for #{cmake.inspect}). " \
          "Install cmake (>= 3.16) or set the CMAKE env var."
  end

  webview_build_dir = File.join(spec.build_dir, 'build')
  lib_ext = is_windows && toolchains.include?('visualcpp') ? '.lib' : '.a'
  lib_name = is_windows && toolchains.include?('visualcpp') ? 'webview_static' : 'webview'
  webview_lib = File.join(webview_build_dir, 'core', "lib#{lib_name}#{lib_ext}")
  # MSVC produces "webview_static.lib" without a "lib" prefix.
  webview_lib = File.join(webview_build_dir, 'core', "#{lib_name}#{lib_ext}") if is_windows && toolchains.include?('visualcpp')

  FileUtils.mkdir_p(webview_build_dir)

  configure_args = [
    "-S \"#{webview_dir}\"",
    "-B \"#{webview_build_dir}\"",
    '-DWEBVIEW_BUILD_STATIC_LIBRARY=ON',
    '-DWEBVIEW_BUILD_SHARED_LIBRARY=OFF',
    '-DWEBVIEW_BUILD_TESTS=OFF',
    '-DWEBVIEW_BUILD_EXAMPLES=OFF',
    '-DWEBVIEW_BUILD_DOCS=OFF',
    '-DWEBVIEW_BUILD_AMALGAMATION=OFF',
    '-DWEBVIEW_ENABLE_PACKAGING=OFF',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_POSITION_INDEPENDENT_CODE=ON'
  ]
  configure_args << "-DCMAKE_C_COMPILER=\"#{cc_command}\""    unless cc_command.empty?
  configure_args << "-DCMAKE_CXX_COMPILER=\"#{cxx_command}\"" unless cxx_command.empty?

  # Configure once (CMake itself is idempotent on no-op reconfigures, but
  # skipping the second invocation keeps the build log clean).
  unless File.exist?(File.join(webview_build_dir, 'CMakeCache.txt'))
    sh "#{cmake} #{configure_args.join(' ')}"
  end

  build_args = ["--build \"#{webview_build_dir}\"",
                '--target webview_core_static',
                '--config Release']
  jobs = ENV['MRUBY_WEBVIEW_JOBS'] || ENV['JOBS']
  build_args << "--parallel #{jobs}" if jobs
  sh "#{cmake} #{build_args.join(' ')}"

  unless File.exist?(webview_lib)
    abort "[mruby-webview] expected static library not found at #{webview_lib} after CMake build."
  end

  # Pass the static archive directly to the link line. Putting it in
  # flags_before_libraries ensures it appears before the system libraries it
  # depends on (gtk/webkit/etc.), so the linker resolves their symbols in the
  # correct order.
  spec.linker.flags_before_libraries << webview_lib
end
