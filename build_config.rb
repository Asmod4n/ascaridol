MRuby::Build.new do |conf|
  conf.toolchain

  #conf.enable_sanitizer 'address'
  conf.cc.defines  << 'MRB_UTF8_STRING' << 'MRB_HIGH_PROFILE' << 'MRB_USE_DEBUG_HOOK'
  conf.cxx.defines << 'MRB_UTF8_STRING' << 'MRB_HIGH_PROFILE' << 'MRB_USE_DEBUG_HOOK'
  #conf.enable_test
  conf.enable_debug
  conf.gem core: 'mruby-bin-mirb'
  conf.gem(File.expand_path(File.dirname(__FILE__))) do |ascaridol|
    ascaridol.rbfiles << "../example/menu.rb"
  end
end