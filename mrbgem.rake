MRuby::Gem::Specification.new('mruby-webview') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'Idiomatic Ruby bindings for the webview/webview library'
  spec.version = '0.1.0'

  spec.add_dependency 'mruby-fast-json'
  spec.add_dependency 'mruby-c-ext-helpers'
  spec.add_dependency 'mruby-cbor'
  spec.add_dependency 'mruby-lmdb'
  spec.add_dependency 'mruby-uri-parser'
  spec.add_dependency 'mruby-mustache', github: 'Asmod4n/mruby-mustache', branch: 'main'
  spec.add_dependency 'typedargs'
  spec.add_dependency 'mruby-string-ext', core: 'mruby-string-ext'
  spec.add_dependency 'mruby-hash-ext',   core: 'mruby-hash-ext'
  spec.add_dependency 'mruby-symbol-ext', core: 'mruby-symbol-ext'
  spec.add_dependency 'mruby-error',      core: 'mruby-error'
  spec.add_dependency 'mruby-proc-ext',   core: 'mruby-proc-ext'

  # mruby-fast-json requires UTF-8 string support.
  spec.cc.defines  << 'MRB_UTF8_STRING'
  spec.cxx.defines << 'MRB_UTF8_STRING'

  # ------------------------------------------------------------------------
  # webview source: shipped as a git submodule under vendor/webview. We
  # compile its full C++ implementation directly into our single .cc
  # translation unit (no WEBVIEW_HEADER, no WEBVIEW_STATIC) so the
  # bindings can use the C++ engine class (webview::webview) — std::string
  # parameters, std::function dispatch / bind callbacks, etc. No CMake
  # build needed; webview's own master `core/src/webview.cc` is just a
  # `#include "webview/webview.h"` shim, and our src/mrb_webview.cc does
  # exactly the same include without the WEBVIEW_HEADER guard.
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
  # Detect the *target* platform we're building mruby for.
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

  if is_windows
    spec.linker.libraries.concat(%w[advapi32 ole32 shell32 shlwapi user32 version])
      pkg_dir = Dir.glob(File.join(spec.dir, 'packages', 'Microsoft.Web.WebView2.*')).max
      pkg_dir ||= ENV['WEBVIEW2_SDK']
      abort "[mruby-webview] WebView2 SDK not found — run: nuget install Microsoft.Web.WebView2 -OutputDirectory packages" unless pkg_dir

      sdk_inc = File.join(pkg_dir, 'build', 'native', 'include')
      abort "[mruby-webview] WebView2.h not found at #{sdk_inc}" unless File.exist?(File.join(sdk_inc, 'WebView2.h'))

      spec.cc.include_paths  << sdk_inc
      spec.cxx.include_paths << sdk_inc
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
  end

  unless spec.cxx.flags.flatten.any? { |f| f =~ /-std=c\+\+/ }
    if is_windows
      spec.cxx.flags << '/std:c++20'
    else
      spec.cxx.flags << '-std=c++20'
    end
  end

  spec.bins = %w(hypha.mrb)
end
