# RSpec Rails Boundaries

Source: https://www.betterspecs.org/
Checked: 2026-06-20
Update trigger: Rails/RSpec major release, request spec convention change, or quarterly review.

Use this for request specs, model/service specs, external HTTP stubs, rake
tasks, and service object examples.

## Test Observable Behavior

Prefer specs that exercise the public boundary:

- Request specs for API endpoints.
- Model and service specs for business behavior.
- Integration/system specs where the user-visible flow matters.

Do not add new controller specs. When touching old controller specs, migrate the
coverage into request specs and remove the controller spec.

## Stub External Requests

Use WebMock or VCR for HTTP calls:

```ruby
before do
  stub_request(:get, 'https://api.example.com/data')
    .to_return(body: '{"result": "ok"}')
end
```

Never let specs depend on live third-party services. Live dependencies make
tests flaky, slow, and hard to reproduce.

## Suppress Noisy Output

Use the `suppress_output` helper from `spec/support/output_helpers.rb` for rake
tasks:

```ruby
subject(:seed) do
  suppress_output { Rake::Task['db:seed'].invoke }
end
```

## Testing Services

Pattern for service objects:

```ruby
RSpec.describe MyService do
  subject(:result) { described_class.call(**params) }

  let(:params) { { query: query, option: option } }
  let(:query) { 'default' }
  let(:option) { true }

  describe '.call' do
    context 'when valid input' do
      it 'returns success' do
        expect(result).to be_success
      end
    end

    context 'when invalid input' do
      let(:query) { nil }

      it 'returns failure' do
        expect(result).to be_failure
      end
    end
  end
end
```
