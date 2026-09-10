import pathlib
import tomllib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class HerdrAgentPickerTests(unittest.TestCase):
    def setUp(self):
        config_dir = ROOT / 'home/config/herdr'
        self.herdr = tomllib.loads((config_dir / 'config.toml').read_text())
        self.picker = tomllib.loads(
            (config_dir / 'agent-picker.toml').read_text()
        )

    def test_only_live_agents_are_enabled(self):
        self.assertEqual(self.picker['sources'], {
            'agents': True,
            'open_workspaces': False,
            'herdr_plus_projects': False,
            'herdr_plus_quick_actions': False,
            'zoxide': False,
            'roots': False,
            'servers': False,
            'sessions': False,
        })
        self.assertFalse(self.picker['jump_back']['enabled'])
        self.assertFalse(self.picker['jump_back']['pin_previous'])
        self.assertFalse(self.picker['picker']['create_missing'])

    def test_fuzzy_matching_and_sidebar_order(self):
        self.assertEqual(self.picker['picker']['engine'], 'nucleo')
        self.assertEqual(self.picker['picker']['agent_sort'], 'herdr')
        self.assertEqual(self.picker['picker']['source_order'], ['agent'])
        self.assertFalse(self.picker['picker']['check_updates'])

    def test_dedicated_shortcut_preserves_existing_navigation(self):
        commands = self.herdr['keys']['command']
        bindings = [c for c in commands if c['key'] == 'prefix+a']
        self.assertEqual(len(bindings), 1)
        self.assertEqual(bindings[0]['type'], 'plugin_action')
        self.assertEqual(bindings[0]['command'], 'herdr-navigator.open')
        self.assertEqual(self.herdr['keys']['goto'], 'prefix+g')
        directions = [('left', 'h'), ('down', 'j'), ('up', 'k'), ('right', 'l')]
        for direction, key in directions:
            action = f'willfish.herdr-navigator.{direction}'
            self.assertTrue(any(
                command['key'] == f'alt+{key}'
                and command['command'] == action
                for command in commands
            ))


if __name__ == '__main__':
    unittest.main()
