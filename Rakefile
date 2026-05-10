require 'rake'
require 'fileutils'

MRUBY_CONFIG_PATH = File.expand_path(ENV["MRUBY_CONFIG"] || "build_config.rb")
DEFAULT_SCRIPT    = File.expand_path("tools/hypha/stub.rb")
EMBED_SOURCE      = File.expand_path("tools/hypha/main.c")

# Resolve which Ruby script to embed. Argument wins over env var wins over default.
def resolve_script(arg)
  candidate = arg || ENV["HYPHA_SCRIPT"] || DEFAULT_SCRIPT
  path = File.expand_path(candidate)
  unless File.file?(path)
    abort "script not found: #{path}"
  end
  path
end

# Locate mrbc. mruby's build puts host binaries in mruby/build/host/bin and
# also creates a wrapper in mruby/bin (an .exe on most platforms, a .bat
# wrapper on Windows). We check all reasonable locations.
def mrbc_path
  candidates = [
    "mruby/bin/mrbc",
    "mruby/bin/mrbc.exe",
    "mruby/bin/mrbc.bat",
    "mruby/build/host/bin/mrbc",
    "mruby/build/host/bin/mrbc.exe",
  ]
  found = candidates.map { |p| File.expand_path(p) }.find { |p| File.exist?(p) }
  abort "mrbc not found; run `rake compile` once first" unless found
  found
end

file :mruby do
  unless File.directory?('mruby')
    sh "git clone --depth=1 https://github.com/mruby/mruby.git"
  end
end

desc "regenerate the no-app-embedded stub"
task :regen_stub => :mruby do
  stub_rb = File.expand_path("tools/hypha/stub.rb")
  abort "stub.rb not found" unless File.exist?(stub_rb)
  sh %Q{"#{mrbc_path}" -Bhypha_main -o "#{EMBED_SOURCE}" "#{stub_rb}"}
end

desc "embed Ruby script into src/main.c (default: example/hello.rb)"
task :embed, [:script] => :mruby do |_, args|
  script = resolve_script(args[:script])
  puts "Embedding #{script} -> #{EMBED_SOURCE}"
  sh %Q{"#{mrbc_path}" -Bhypha_main -o "#{EMBED_SOURCE}" "#{script}"}
end

desc "compile binary (optional arg: path to .rb script)"
task :compile, [:script] => :mruby do |_, args|
  # First do a build to make sure mrbc exists, then embed, then build again
  # so the binary picks up the freshly-embedded src/main.c.
  Dir.chdir("mruby") do
    ENV["MRUBY_CONFIG"] = MRUBY_CONFIG_PATH
    sh "rake all"
  end

  Rake::Task[:embed].invoke(args[:script])

  Dir.chdir("mruby") do
    ENV["MRUBY_CONFIG"] = MRUBY_CONFIG_PATH
    sh "rake all"
  end
end

desc "test"
task :test => :mruby do
  Dir.chdir("mruby") do
    ENV["MRUBY_CONFIG"] = MRUBY_CONFIG_PATH
    sh "rake all test"
  end
end

desc "cleanup"
task :clean do
  Dir.chdir("mruby") do
    ENV["MRUBY_CONFIG"] = MRUBY_CONFIG_PATH
    sh "rake deep_clean"
  end
end

task :default => :test