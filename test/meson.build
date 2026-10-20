test('plainc', executable('ctest', 'ctest.c',
                          dependencies: capypdf_dep,
                          link_language: 'c'))

test('cppwrapper', executable('cpptest', 'cpptest.cpp', override_options: ['cpp_std=c++20'], dependencies: capypdf_dep))

test('cffsubset', executable('cffsubsettest', 'cffsubsettest.cpp',
                             dependencies: capypdf_internal_dep),
  args: [meson.project_source_root() / 'testdata' / 'fonts'])

test('syntax', find_program('syntaxchecks.py'))

test('format', find_program('formatcheck.py'))

if get_option('default_library') == 'shared'
  test('Python tests', find_program('capypdftests.py'), args: ['-v'])
endif
