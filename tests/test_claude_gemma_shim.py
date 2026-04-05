import importlib.util
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / 'home' / 'user' / 'claude-gemma-shim.py'
spec = importlib.util.spec_from_file_location('claude_gemma_shim', MODULE_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)


class ParseAnthropicRequestCompatibilityTests(unittest.TestCase):
    def test_ignores_claude_context_management_and_output_config(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'context_management': {},
            'output_config': {},
        }
        parsed = module.parse_anthropic_request(payload)
        self.assertEqual(parsed['messages'], [{'role': 'user', 'content': 'Reply with exactly hello.'}])

    def test_ignores_claude_metadata_and_thinking_objects(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'metadata': {},
            'thinking': {},
        }
        parsed = module.parse_anthropic_request(payload)
        self.assertEqual(parsed['messages'], [{'role': 'user', 'content': 'Reply with exactly hello.'}])

    def test_accepts_system_as_text_blocks(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'system': [
                {'type': 'text', 'text': 'You are concise.'},
                {'type': 'text', 'text': 'Answer plainly.'},
            ],
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
        }
        parsed = module.parse_anthropic_request(payload)
        self.assertEqual(parsed['system'], 'You are concise.Answer plainly.')

    def test_accepts_tool_schema_with_draft_identifier(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'tools': [{
                'name': 'Read',
                'description': 'Read a file.',
                'input_schema': {
                    '$schema': 'http://json-schema.org/draft-07/schema#',
                    'type': 'object',
                    'properties': {'file_path': {'type': 'string'}},
                    'required': ['file_path'],
                    'additionalProperties': False,
                },
            }],
        }
        parsed = module.parse_anthropic_request(payload)
        self.assertEqual(parsed['tools'][0]['function']['name'], 'Read')

    def test_accepts_tool_schema_with_array_length_bounds(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'tools': [{
                'name': 'AskUserQuestion',
                'description': 'Ask the user a follow-up.',
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'questions': {
                            'type': 'array',
                            'items': {'type': 'string'},
                            'minItems': 1,
                            'maxItems': 3,
                        },
                    },
                    'required': ['questions'],
                    'additionalProperties': False,
                },
            }],
        }
        parsed = module.parse_anthropic_request(payload)
        questions_schema = parsed['tools'][0]['function']['parameters']['properties']['questions']
        self.assertEqual(questions_schema['minItems'], 1)
        self.assertEqual(questions_schema['maxItems'], 3)

    def test_accepts_tool_schema_with_property_names(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'tools': [{
                'name': 'AskUserQuestion',
                'description': 'Ask the user a follow-up.',
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'answers': {
                            'type': 'object',
                            'propertyNames': {'type': 'string', 'pattern': '^[a-z0-9_-]+$'},
                            'additionalProperties': {'type': 'string'},
                        },
                    },
                    'required': ['answers'],
                    'additionalProperties': False,
                },
            }],
        }
        parsed = module.parse_anthropic_request(payload)
        answers_schema = parsed['tools'][0]['function']['parameters']['properties']['answers']
        self.assertEqual(answers_schema['propertyNames']['pattern'], '^[a-z0-9_-]+$')

    def test_accepts_additional_properties_schema_without_explicit_type(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'tools': [{'name': 'GenericMap', 'input_schema': {'type': 'object', 'additionalProperties': {}}}],
        }
        parsed = module.parse_anthropic_request(payload)
        self.assertEqual(parsed['tools'][0]['function']['parameters']['additionalProperties'], {})

    def test_accepts_tool_result_block_with_cache_control(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [
                {
                    'role': 'assistant',
                    'content': [
                        {'type': 'tool_use', 'id': 'toolu_1', 'name': 'Bash', 'input': {'command': 'pwd'}},
                    ],
                },
                {
                    'role': 'user',
                    'content': [
                        {
                            'type': 'tool_result',
                            'tool_use_id': 'toolu_1',
                            'content': '/tmp',
                            'is_error': False,
                            'cache_control': {'type': 'ephemeral'},
                        }
                    ],
                },
            ],
        }
        parsed = module.parse_anthropic_request(payload)
        self.assertEqual(parsed['messages'][1]['role'], 'tool')
        self.assertEqual(parsed['messages'][1]['tool_call_id'], 'toolu_1')
        self.assertEqual(parsed['messages'][1]['content'], '/tmp')

    def test_accepts_any_of_schema_composition(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'tools': [{
                'name': 'StatusTool',
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'status': {
                            'anyOf': [
                                {'type': 'string'},
                                {'type': 'null'},
                            ]
                        }
                    }
                },
            }],
        }
        parsed = module.parse_anthropic_request(payload)
        status_schema = parsed['tools'][0]['function']['parameters']['properties']['status']
        self.assertEqual(len(status_schema['anyOf']), 2)

    def test_rejects_one_of_schema_composition(self):
        payload = {
            'model': 'claude-sonnet-4-6',
            'max_tokens': 32,
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'Reply with exactly hello.'}]}],
            'tools': [{
                'name': 'BadTool',
                'input_schema': {'type': 'object', 'properties': {'value': {'oneOf': [{'type': 'string'}, {'type': 'integer'}]}}},
            }],
        }
        with self.assertRaises(module.RequestValidationError) as exc:
            module.parse_anthropic_request(payload)
        self.assertIn('unsupported JSON Schema keyword', exc.exception.message)
        self.assertIn('oneOf', exc.exception.message)


if __name__ == '__main__':
    unittest.main()
