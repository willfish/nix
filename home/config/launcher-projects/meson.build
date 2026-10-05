project('launcher-projects', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('gio-2.0'), dependency('yyjson')]
core = static_library('projects', ['src/path.c', 'src/catalogue.c', 'src/open.c'], include_directories: 'include', dependencies: deps)
projects_dep = declare_dependency(link_with: core, include_directories: 'include', dependencies: deps)
executable('launcher-projects', 'src/main.c', dependencies: projects_dep, install: true)
if get_option('fixtures')
  fixture_link_args = host_machine.system() == 'linux' ? ['-Wl,--wrap=readdir64'] : []
  executable('projects-fixture', 'tests/fixture.c', dependencies: projects_dep, link_args: fixture_link_args)
endif
