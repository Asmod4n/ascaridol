MRuby::Build.new do |conf|
  conf.toolchain

  conf.enable_sanitizer 'address'
  conf.cc.defines  << 'MRB_UTF8_STRING' << 'MRB_HIGH_PROFILE'
  conf.cxx.defines << 'MRB_UTF8_STRING' << 'MRB_HIGH_PROFILE'
  conf.enable_test
  conf.enable_debug
  conf.gem File.expand_path(File.dirname(__FILE__))
end