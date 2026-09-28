"""Cloud speech catalogues, preferences and local-service isolation."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
import urllib.error
import wave

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / 'home/config/voice'))
from voice_audio import LocalAudio, DEEPGRAM_VOICES
from voice_menu import rows_for, prompt_for
from voice_controller import Controller, dispatch


class DeepgramSpeechTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.runtime = Path(self.tmp.name)
        self.enterContext(patch.dict(
            os.environ, {'DEEPGRAM_API_KEY': 'test-key'}))
        self.config = {
            'voice_preferences_path': str(self.runtime / 'voice-mode')
        }
        self.audio = LocalAudio(self.runtime, self.config, engines=Mock())

    def test_catalogue_and_preferences_are_separate(self):
        self.assertEqual(self.audio.speech_backend, 'deepgram')
        self.assertEqual(len(DEEPGRAM_VOICES), 41)
        self.assertNotIn('samantha', self.audio.status()['voices'])
        self.audio.set_voice('apollo')
        with self.assertRaises(RuntimeError):
            self.audio.set_voice('samantha')
        self.audio.set_speech_backend('local')
        self.assertNotIn('apollo', self.audio.status()['voices'])
        self.audio.set_voice('samantha')
        restored = LocalAudio(self.runtime, self.config)
        self.assertEqual(restored.speech_backend, 'local')
        restored.set_speech_backend('deepgram')
        self.assertEqual(restored.status()['selected_voice'], 'apollo')

    def test_cloud_only_and_no_speech_hosts(self):
        cloud = LocalAudio(self.runtime, {'tts_enabled': False})
        self.assertEqual(cloud.speech_backends, {'deepgram': 'Deepgram'})
        self.assertNotIn(
            ('menu:speech', 'Choose speech'), rows_for(cloud.status(), 'menu'))
        with patch.dict(os.environ, {'DEEPGRAM_API_KEY': ''}):
            silent = LocalAudio(self.runtime, {'tts_enabled': False})
            self.assertEqual(silent.speech_backends, {})
            self.assertEqual(silent.status()['voices'], {})
            local = LocalAudio(self.runtime, {})
            self.assertEqual(local.speech_backend, 'local')

    def test_menu_uses_active_catalogue_and_selected_backend(self):
        status = self.audio.status()
        self.assertIn(
            ('menu:speech', 'Choose speech'), rows_for(status, 'menu'))
        voices = rows_for(status, 'voices')
        self.assertTrue(any(action == 'voice:thalia' for action, _ in voices))
        self.assertFalse(any(
            action == 'voice:samantha' for action, _ in voices))
        self.assertEqual(prompt_for(status, 'speech'), 'Speech backend')
        self.assertTrue(any(
            action == 'speech:deepgram'
            for action, _ in rows_for(status, 'speech')))

    @staticmethod
    def wav():
        out = io.BytesIO()
        with wave.open(out, 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(24000)
            wav.writeframes(b'\0\0' * 24)
        return out.getvalue()

    def test_request_uses_wav_and_never_acquires_gpu(self):
        with patch(
            'urllib.request.urlopen', return_value=io.BytesIO(self.wav())
        ) as http:
            params, frames = self.audio._synthesize(
                'Hello.', threading.Event(),
                DEEPGRAM_VOICES['thalia']['options'])
        request = http.call_args.args[0]
        self.assertIn('model=aura-2-thalia-en', request.full_url)
        self.assertIn('container=wav', request.full_url)
        self.assertIn('mip_opt_out=true', request.full_url)
        self.assertEqual(json.loads(request.data), {'text': 'Hello.'})
        self.assertEqual(params.framerate, 24000)
        self.assertEqual(len(frames), 48)
        self.audio.engines.acquire.assert_not_called()

    def test_playback_modes_bypass_local_readiness(self):
        for mode in ('buffered', 'streaming'):
            with self.subTest(mode=mode):
                self.audio.config['playback_mode'] = mode
                with (
                    patch(
                        'urllib.request.urlopen',
                        side_effect=lambda *a, **k: io.BytesIO(self.wav())),
                    patch('voice_audio.subprocess.Popen') as player,
                    patch.object(self.audio, 'wait_ready') as ready,
                ):
                    player.return_value.wait.return_value = 0
                    self.audio.speak('Hello.', threading.Event())
                ready.assert_not_called()
                self.audio.engines.acquire.assert_not_called()

    def test_switch_preserves_pending_dictation(self):
        app = Controller(self.runtime, Mock(), self.audio, Mock())
        app.pending = 'Keep this draft'
        dispatch(app, {'action': 'speech:local'})
        self.assertEqual(app.pending, 'Keep this draft')
        self.assertEqual(self.audio.speech_backend, 'local')

    def test_cloud_status_ignores_inactive_local_tts_health(self):
        self.audio._backend_state('tts', 'error', 'Local server unavailable')
        status = self.audio.status()
        self.assertNotIn('tts', status['backends'])
        self.assertNotIn('tts', status['backend_errors'])

    def test_error_does_not_expose_response_or_credentials(self):
        error = urllib.error.HTTPError(
            'https://api.deepgram.com', 401, 'private', {},
            io.BytesIO(b'private'))
        with patch('urllib.request.urlopen', side_effect=error):
            with self.assertRaisesRegex(
                RuntimeError, r'^Deepgram speech failed \(HTTP 401\)$'
            ):
                self.audio._synthesize_deepgram(
                    'Hello.', threading.Event(), 'aura-2-thalia-en')

    def test_cancelled_request_does_not_send(self):
        cancelled = threading.Event()
        cancelled.set()
        with patch('urllib.request.urlopen') as http:
            self.assertIsNone(self.audio._synthesize_deepgram(
                'Hello.', cancelled, 'aura-2-thalia-en'))
        http.assert_not_called()

    def test_controller_cloud_playback_and_auto_do_not_pin_gpu(self):
        engines = Mock()
        engines._states = {'tts': object()}
        audio = Mock(speech_backend='deepgram')
        app = Controller(self.runtime, Mock(), audio, Mock(), engines=engines)
        app.set_auto(True)
        engines.ensure_resident.assert_not_called()
        engines.retire.assert_called_once_with('tts')
        app._speak('Hello.', threading.Event())
        engines.acquire.assert_not_called()
        audio.speak.assert_called_once()
        app.set_auto(False)
        audio.stop.assert_called()
        audio.speech_backend = 'local'
        app.set_auto(True)
        engines.ensure_resident.assert_called_once_with('tts')


if __name__ == '__main__':
    unittest.main()
