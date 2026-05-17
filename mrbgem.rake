MRuby::Gem::Specification.new('hypha-mrb') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'A small Framework to build native desktop apps in mruby'
  spec.version = '0.2.0'

  spec.add_dependency 'mruby-fast-json'
  spec.add_dependency 'mruby-c-ext-helpers'
  spec.add_dependency 'mruby-cbor'
  spec.add_dependency 'mruby-lmdb'
  spec.add_dependency 'mruby-uri-parser'
  spec.add_dependency 'mruby-mustache', github: 'Asmod4n/mruby-mustache', branch: 'main'
  spec.add_dependency 'typedargs'
  spec.add_dependency 'mruby-proc-irep-ext'
  spec.add_dependency 'mruby-class-ext',  core: 'mruby-class-ext'
  spec.add_dependency 'mruby-string-ext', core: 'mruby-string-ext'
  spec.add_dependency 'mruby-hash-ext',   core: 'mruby-hash-ext'
  spec.add_dependency 'mruby-symbol-ext', core: 'mruby-symbol-ext'
  spec.add_dependency 'mruby-error',      core: 'mruby-error'
  spec.add_dependency 'mruby-proc-ext',   core: 'mruby-proc-ext'
  spec.add_dependency 'mruby-io',         core: 'mruby-io'
  spec.add_dependency 'mruby-socket',     core: 'mruby-socket'


  # ------------------------------------------------------------------------
  # webview source: shipped as a git submodule under vendor/webview.
  # ------------------------------------------------------------------------
  webview_dir = ENV['HYPHA_WEBVIEW_DIR'] || File.join(spec.dir, 'vendor', 'webview')
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
      [hypha-mrb] webview source not found at #{webview_dir}.
      Run `git submodule update --init --recursive` in the gem directory,
      or set HYPHA_WEBVIEW_DIR to point at an existing webview checkout.
    MSG
  end

  spec.cc.include_paths  << webview_inc
  spec.cxx.include_paths << webview_inc

  # ------------------------------------------------------------------------
  # Detect the *target* platform we're building mruby for.
  # ------------------------------------------------------------------------
  debug = spec.build.cc.defines.include?('MRB_DEBUG')
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
    unless debug
        spec.linker.flags << '/SUBSYSTEM:WINDOWS'
        spec.linker.flags << '/ENTRY:mainCRTStartup'
    end
    spec.linker.libraries.concat(%w[advapi32 ole32 shell32 shlwapi user32 version])

    # ----------------------------------------------------------------------
    # WebView2 SDK: dotnet restore against a throwaway csproj. dotnet
    # fetches the latest Microsoft.Web.WebView2, verifies Microsoft's
    # signature, and extracts to ./packages/microsoft.web.webview2/<ver>/.
    # Build only reads native headers — no .NET runtime, no C# code.
    # Override with WEBVIEW2_SDK=/path/to/microsoft.web.webview2/<version>
    # ----------------------------------------------------------------------
    sdk_header_rel = File.join('build', 'native', 'include', 'WebView2.h')
    pkg_dir        = ENV['WEBVIEW2_SDK']

    unless pkg_dir
      require 'tmpdir'

      packages_root = File.join(spec.dir, 'packages')
      FileUtils.mkdir_p(packages_root)

      Dir.mktmpdir('hypha-webview2-restore') do |tmp|
        File.write(File.join(tmp, 'webview2.csproj'), <<~XML)
          <Project Sdk="Microsoft.NET.Sdk">
            <PropertyGroup>
              <TargetFramework>net8.0</TargetFramework>
              <NoWarn>NU1701</NoWarn>
            </PropertyGroup>
            <ItemGroup>
              <PackageReference Include="Microsoft.Web.WebView2" Version="*" />
            </ItemGroup>
          </Project>
        XML
        sh 'dotnet', 'restore', tmp, '--packages', packages_root
      end

      pkg_dir = Dir.glob(File.join(packages_root, 'microsoft.web.webview2', '*'))
                   .reject { |d| File.basename(d).include?('-') }
                   .select { |d| File.exist?(File.join(d, sdk_header_rel)) }
                   .max_by { |d| File.basename(d).split('.').map(&:to_i) }
      abort "[hypha-mrb] dotnet restore ran but no microsoft.web.webview2 in #{packages_root}" unless pkg_dir
      puts "[hypha-mrb] using WebView2 SDK at #{pkg_dir}"
    end

    sdk_inc = File.join(pkg_dir, 'build', 'native', 'include')
    abort "[hypha-mrb] WebView2.h not found at #{sdk_inc}" unless File.exist?(File.join(sdk_inc, 'WebView2.h'))

    spec.cc.include_paths  << sdk_inc
    spec.cxx.include_paths << sdk_inc

    # ----------------------------------------------------------------------
    # Win32 manifest: UTF-8 codepage, long paths, per-monitor DPI v2,
    # Common-Controls 6 dependency for themed menus.
    # ----------------------------------------------------------------------
    rc_src     = File.join(spec.dir, 'data', 'mruby-webview.rc')
    rc_obj_dir = File.join(build.build_dir, 'mrbgems', spec.name)
    FileUtils.mkdir_p(rc_obj_dir)

    rc_obj = if toolchains.include?('visualcpp')
      File.join(rc_obj_dir, 'mruby-webview.res')
    else
      File.join(rc_obj_dir, 'mruby-webview.res.o')
    end

    if toolchains.include?('visualcpp')
      unless File.exist?(rc_obj) && File.mtime(rc_obj) > File.mtime(rc_src)
        sh "rc.exe", "/nologo", "/I", File.dirname(rc_src), "/fo", rc_obj, rc_src
      end
    else
      windres = ENV['WINDRES'] || (cc_command =~ /(.+?-)gcc(\.exe)?$/ ? "#{$1}windres" : 'windres')
      unless File.exist?(rc_obj) && File.mtime(rc_obj) > File.mtime(rc_src)
        sh windres, "--include-dir=#{File.dirname(rc_src)}", "-i", rc_src, "-o", rc_obj, "-O", "coff"
      end
    end

    spec.linker.flags_before_libraries << rc_obj

  elsif is_darwin
    spec.linker.flags_after_libraries.concat(%w[-framework WebKit -framework Cocoa])
    spec.linker.libraries << 'c++'

  else
    pkg = ENV['HYPHA_WEBVIEW_PKG']
    pkg ||= %w[gtk4 webkitgtk-6.0].all?       { |p| system("pkg-config --exists #{p}") } ? 'gtk4 webkitgtk-6.0'      : nil
    pkg ||= %w[gtk+-3.0 webkit2gtk-4.1].all?  { |p| system("pkg-config --exists #{p}") } ? 'gtk+-3.0 webkit2gtk-4.1' : nil
    pkg ||= %w[gtk+-3.0 webkit2gtk-4.0].all?  { |p| system("pkg-config --exists #{p}") } ? 'gtk+-3.0 webkit2gtk-4.0' : nil

    abort <<~MSG unless pkg
      [hypha-mrb] no GTK + WebKitGTK development packages found via pkg-config.
      Install one of:
        - gtk4 + webkitgtk-6.0   (Debian/Ubuntu: libgtk-4-dev libwebkitgtk-6.0-dev)
        - gtk+-3.0 + webkit2gtk-4.1   (libgtk-3-dev libwebkit2gtk-4.1-dev)
        - gtk+-3.0 + webkit2gtk-4.0   (libgtk-3-dev libwebkit2gtk-4.0-dev)
      Or set HYPHA_WEBVIEW_PKG to a custom pkg-config package list.
    MSG

    `pkg-config --cflags #{pkg}`.strip.split(/\s+/).reject(&:empty?).each do |f|
      case f
      when /\A-I(.+)/ then spec.cc.include_paths << $1; spec.cxx.include_paths << $1
      when /\A-D(.+)/ then spec.cc.defines       << $1; spec.cxx.defines       << $1
      else                 spec.cc.flags         << f;  spec.cxx.flags         << f
      end
    end

    spec.linker.flags_after_libraries.concat(`pkg-config --libs #{pkg}`.strip.split(/\s+/).reject(&:empty?))
    spec.linker.libraries << 'stdc++'
    spec.linker.libraries << 'pthread'
  end

  if is_windows
      spec.cxx.flags << '/std:c++20'
  else
      spec.cxx.flags << '-std=c++20'
  end

  spec.bins = %w(hypha)
end
