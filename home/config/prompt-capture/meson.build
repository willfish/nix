project('prompt-capture-mitm', 'c', version: '0.1.0',
  meson_version: '>=1.1',
  default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
python_executable = get_option('python_executable')
site_packages = get_option('site_packages')
if python_executable == '' or site_packages == ''
  discover = run_command(find_program('python3'), '-c', '''
import glob, os, subprocess, sys
env_py = os.environ.get("PROMPT_CAPTURE_PYTHON", "")
env_site = os.environ.get("PROMPT_CAPTURE_SITE", "")
if env_py and env_site and os.path.exists(os.path.join(env_site, "mitmproxy")):
    print(env_py)
    print(env_site)
    raise SystemExit
cands = sorted(glob.glob("/nix/store/*-python3*-env/bin/python3"), key=os.path.getmtime, reverse=True)
for py in cands:
    probe = subprocess.run([py, "-c", "import mitmproxy, sysconfig; print(sysconfig.get_path('purelib'))"], capture_output=True, text=True)
    if probe.returncode == 0 and os.path.isdir(probe.stdout.strip()):
        print(py)
        print(probe.stdout.strip())
        raise SystemExit
sys.exit("mitmproxy interpreter not found; pass -Dpython_executable and -Dsite_packages")
''', check: true)
  lines = discover.stdout().strip().split('\n')
  if python_executable == ''
    python_executable = lines[0]
  endif
  if site_packages == ''
    site_packages = lines[1]
  endif
endif
config = configuration_data()
config.set_quoted('PYTHON_EXECUTABLE', python_executable)
config.set_quoted('SITE_PACKAGES', site_packages)
configure_file(output: 'config.h', configuration: config)
executable('prompt-capture-mitm', 'main.c',
  dependencies: dependency('python3-embed'), install: true)
if get_option('fixtures')
  script = join_paths(meson.current_source_dir(), '../../user/prompt-capture.sh')
  executable('prompt-capture-checks', ['tests/check.c', 'tests/run.c'],
    c_args: ['-D_DEFAULT_SOURCE', '-DSCRIPT="' + script + '"'])
  executable('capture-ready', 'tests/ready.c', c_args: ['-D_DEFAULT_SOURCE'])
endif
