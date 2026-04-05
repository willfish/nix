import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / 'home' / 'user' / 'claude-gemma-launcher.py'
if not MODULE_PATH.exists():
    raise AssertionError(f'missing launcher module: {MODULE_PATH}')

spec = importlib.util.spec_from_file_location('claude_gemma_launcher', MODULE_PATH)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
assert spec.loader is not None
spec.loader.exec_module(module)


class LauncherConfigTests(unittest.TestCase):
    def test_runtime_config_uses_ephemeral_ports_and_private_paths(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            runtime = module.build_runtime_config(runtime_root=Path(tmpdir))

            self.assertEqual(runtime.host, '127.0.0.1')
            self.assertNotEqual(runtime.model_port, runtime.shim_port)
            self.assertGreater(runtime.model_port, 0)
            self.assertGreater(runtime.shim_port, 0)
            self.assertTrue(str(runtime.runtime_root).startswith(tmpdir))
            self.assertEqual(runtime.llm_env['LLM_GEMMA_HOST'], '127.0.0.1')
            self.assertEqual(runtime.llm_env['LLM_GEMMA_PORT'], str(runtime.model_port))
            self.assertEqual(runtime.llm_env['LLM_GEMMA_CTX_SIZE'], '40960')
            self.assertEqual(runtime.shim_env['CLAUDE_GEMMA_SHIM_HOST'], '127.0.0.1')
            self.assertEqual(runtime.shim_env['CLAUDE_GEMMA_SHIM_PORT'], str(runtime.shim_port))
            self.assertEqual(runtime.shim_env['LLM_GEMMA_HOST'], '127.0.0.1')
            self.assertEqual(runtime.shim_env['LLM_GEMMA_PORT'], str(runtime.model_port))
            self.assertEqual(runtime.claude_env['ANTHROPIC_BASE_URL'], f'http://127.0.0.1:{runtime.shim_port}')

    def test_runtime_config_allocates_new_ports_each_time(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            first = module.build_runtime_config(runtime_root=Path(tmpdir))
            second = module.build_runtime_config(runtime_root=Path(tmpdir))

            self.assertNotEqual(first.model_port, second.model_port)
            self.assertNotEqual(first.shim_port, second.shim_port)


class LauncherReadinessTests(unittest.TestCase):
    def test_wait_for_http_accepts_http_404_as_ready(self):
        class FakeResponse:
            def __init__(self, status):
                self.status = status
            def __enter__(self):
                return self
            def __exit__(self, exc_type, exc, tb):
                return False

        from urllib.error import HTTPError

        def fake_urlopen(_url, timeout=2):
            raise HTTPError(url='http://127.0.0.1', code=404, msg='Not Found', hdrs=None, fp=None)

        original = module.urlopen
        module.urlopen = fake_urlopen
        try:
            module.wait_for_http('http://127.0.0.1', timeout_seconds=0.01)
        finally:
            module.urlopen = original


if __name__ == '__main__':
    unittest.main()
