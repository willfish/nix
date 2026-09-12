"""Guard the private input boundary without reading secret values."""
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PrivateConfigurationTest(unittest.TestCase):
    def test_private_input_is_pinned_over_ssh(self):
        lock = json.loads((ROOT / 'flake.lock').read_text())
        node = lock['nodes'][lock['nodes']['root']['inputs']['nix-config']]
        for source in ('locked', 'original'):
            self.assertEqual(node[source]['type'], 'git')
            self.assertEqual(node[source]['url'],
                             'ssh://git@github.com/willfish/nix-config.git')
        self.assertRegex(node['locked']['rev'], r'^[0-9a-f]{40}$')

    def test_both_configuration_families_import_private_modules(self):
        flake = (ROOT / 'flake.nix').read_text()
        self.assertIn('nix-config.homeModules.default', flake)
        self.assertIn('nix-config.nixosModules.default', flake)
        self.assertNotIn('./secrets.nix',
                         (ROOT / 'home/user/default.nix').read_text())
        self.assertNotIn('defaultSopsFile',
                         (ROOT / 'system/modules/base.nix').read_text())

    def test_old_secret_sources_are_untracked_and_ignored(self):
        paths = ['.sops.yaml', 'secrets/env.yaml', 'secrets/ssh.yaml']
        tracked = subprocess.check_output(
            ['git', 'ls-files', '--', *paths], cwd=ROOT, text=True)
        self.assertEqual(tracked, '')
        ignored = subprocess.check_output(
            ['git', 'check-ignore', '--', *paths], cwd=ROOT, text=True)
        self.assertEqual(ignored.splitlines(), paths)


if __name__ == '__main__':
    unittest.main()
