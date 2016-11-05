MRuby::Gem::Specification.new('mruby-mucgly') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Tero Isannainen'
  spec.summary = 'mucgly library'
  spec.version = '0.0.1'

  glib_lib = %x{pkg-config --libs glib-2.0}[2..-2]
  glib_inc = %x{pkg-config --cflags glib-2.0}.split.map{|i| i[2..-1]}

  spec.cc.flags = [ENV['CFLAGS'] || %w(-std=gnu11)]
  spec.cc.include_paths += glib_inc

end
