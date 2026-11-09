import json
import sqlite3
from concurrent.futures import ThreadPoolExecutor
import urllib.error

import pytest

from server.ark_summary import ArkFailure, ArkSummarizer, PROMPT
from server.models import Upload
from server.store import DAY
from server.worker import Worker
from test_memory import system, payload

RESULT = dict(topic='Dinosaurs', recap='The child compared a T. rex and a Triceratops.',
              resume_question='Which dinosaur would you draw next?', new_words=['horns'],
              parent_recap='聊过霸王龙和三角龙。', parent_highlight='未观察到', parent_help='未观察到')


class Reply:
    status = 200
    def __init__(self, raw): self.raw = raw
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def read(self, n): return self.raw[:n]


class HTTP:
    def __init__(self, data=None, error=None):
        self.data = data if data is not None else {'choices': [{'finish_reason': 'stop', 'message': {'content': json.dumps(RESULT)}}],
                     'usage': {'prompt_tokens': 160, 'completion_tokens': 70}}
        self.error = error
        self.requests = []
    def open(self, req, timeout):
        self.requests.append((req, timeout))
        if self.error: raise self.error
        return Reply(self.data if isinstance(self.data, bytes) else json.dumps(self.data).encode())


def adapter(tmp_path, http=None, **kw):
    return ArkSummarizer(key='synthetic-private-key', model='synthetic-model',
                         usage_db=tmp_path/'usage.sqlite', opener=http or HTTP(), **kw)


def test_request_is_bounded_nonthinking_json_and_usage_durable(tmp_path, caplog):
    http = HTTP(); a = adapter(tmp_path, http, daily_calls=1)
    assert a.summarize([dict(role='user', text='I like dinosaurs.')]) == RESULT
    req, timeout = http.requests[0]
    data = json.loads(req.data)
    assert data['max_tokens'] == 512 and data['thinking'] == {'type': 'disabled'}
    assert data['response_format'] == {'type': 'json_object'} and data['stream'] is False
    assert timeout == 15 and req.full_url.startswith('https://ark.cn-beijing.volces.com/')
    with sqlite3.connect(a.usage_db) as db:
        assert db.execute('SELECT calls,input_tokens,output_tokens FROM usage').fetchone() == (1,160,70)
    with pytest.raises(ArkFailure): adapter(tmp_path, http, daily_calls=1).summarize([])
    assert len(http.requests) == 1
    assert 'synthetic-private-key' not in caplog.text and 'dinosaurs' not in caplog.text


@pytest.mark.parametrize('failure', ['timeout','429','nonjson','content_nonjson','long','length','schema'])
def test_provider_failure_no_retry_falls_back(system, tmp_path, failure, caplog):
    store, _, a, _, _, _ = system
    p = payload(store); assert store.upload(store.device(a), Upload.model_validate(p))[0] == 200
    http = HTTP()
    if failure == 'timeout': http.error = TimeoutError('private provider error')
    if failure == '429': http.error = urllib.error.HTTPError('https://ark/',429,'private error',{},None)
    if failure == 'nonjson': http.data = b'not JSON private text'
    if failure == 'long': http.data = b'x'*32769
    if failure == 'length': http.data['choices'][0]['finish_reason'] = 'length'
    if failure == 'content_nonjson': http.data['choices'][0]['message']['content'] = 'private non JSON output'
    if failure == 'schema': http.data['choices'][0]['message']['content'] = json.dumps(dict(RESULT, topic=123))
    worker = Worker(store, adapter(tmp_path, http))
    assert worker.once() and not worker.once()
    assert len(http.requests) == 1
    summaries = [s for c in store.snapshot('family-a')['children'] for s in c['summaries']]
    assert summaries[0]['status'] == 'fallback'
    reason = {'long': 'ark_response_limit', 'length': 'ark_incomplete'}.get(failure, 'ark_summary_unavailable')
    assert f'summary_fallback session={p["session_id"][:8]} reason={reason}' in caplog.text
    for private in ('I like dinosaurs.', 'Which dinosaur?', 'Ben', 'Amy', 'private', 'synthetic-private-key'):
        assert private not in caplog.text


def test_long_conversation_chunks_carry_earlier_topic_and_cover_every_character(tmp_path, caplog):
    http = HTTP(); a = adapter(tmp_path, http)
    turns = [dict(role='user', text='Dinosaurs: horn tail plant. ' + '三角龙吃植物。'*500),
             dict(role='assistant', text='A dinosaur has a tail.'), dict(role='user', text='Bye.')]
    assert a.summarize(turns)['topic'] == 'Dinosaurs'
    assert len(http.requests) > 2
    recovered = []
    for i, (request, _) in enumerate(http.requests):
        data = json.loads(request.data)
        text = data['messages'][1]['content']
        assert len(PROMPT.encode()) + len(text.encode()) + 128 <= a.input_tokens
        part = json.loads(text)
        assert (part['previous'] is None) == (i == 0)
        if i:
            assert part['previous']['topic'] == 'Dinosaurs' and part['previous']['new_words'] == ['horns']
            assert all(part['previous'][k] == RESULT[k] for k in ('parent_recap', 'parent_highlight', 'parent_help'))
        recovered.extend(part['turns'])
    assert ''.join(t['text'] for t in recovered) == ''.join(t['text'] for t in turns)
    assert '三角龙' not in caplog.text and 'synthetic-private-key' not in caplog.text
    with sqlite3.connect(a.usage_db) as db:
        assert db.execute('SELECT calls FROM usage').fetchone()[0] == len(http.requests)


def test_input_too_small_and_zero_quota_make_no_http_request(tmp_path):
    http = HTTP(); a = adapter(tmp_path, http, input_tokens=128)
    with pytest.raises(ArkFailure): a.summarize([dict(role='user', text='hello')])
    with pytest.raises(ArkFailure): adapter(tmp_path, http, daily_calls=0).summarize([])
    assert not http.requests


def test_concurrent_instances_cannot_race_daily_limit(tmp_path):
    http = HTTP()
    def call(_):
        try: adapter(tmp_path, http, daily_calls=2).summarize([]); return True
        except ArkFailure: return False
    with ThreadPoolExecutor(max_workers=6) as pool:
        assert sum(pool.map(call, range(6))) == 2
    assert len(http.requests) == 2


def test_latest_card_per_child_deletion_and_retention(system, tmp_path):
    store, _, secret, _, now, _ = system
    ben = next(c for c in store.snapshot('family-a')['children'] if c['profile']['name'] == 'Ben')
    p = payload(store, sid='ben-dinosaurs')
    p['child_id'] = ben['id']
    result = dict(RESULT, new_words=['horn', 'tail', 'plant', 'dinosaur'])
    http = HTTP(); http.data['choices'][0]['message']['content'] = json.dumps(result)
    worker = Worker(store, adapter(tmp_path, http))
    assert store.upload(store.device(secret), Upload.model_validate(p))[0] == 200
    assert worker.once()
    now[0] += DAY
    cards = store.snapshot('family-a')['children']
    card = next(c['summaries'][0] for c in cards if c['id'] == ben['id'])
    assert card['new_words'] == ['horn', 'tail', 'plant'] and card['session_id'] == p['session_id']
    assert len(card['date']) == 10 and card['topic'] == 'Dinosaurs'
    assert not next(c['summaries'] for c in cards if c['profile']['name'] == 'Amy')
    # Completing an older upload later must not replace the latest conversation.
    older = dict(p, session_id='older-late-job', started_at=p['started_at']-DAY)
    assert store.upload(store.device(secret), Upload.model_validate(older))[0] == 200
    assert worker.once()
    assert next(c['summaries'] for c in store.snapshot('family-a')['children'] if c['id'] == ben['id']) == [card]
    now[0] += 30*DAY; store.cleanup()
    assert next(c['summaries'] for c in store.snapshot('family-a')['children'] if c['id'] == ben['id']) == [card]
    assert all(r['transcript'] is None for r in store.records('family-a')['records'])
    assert store.delete('family-a', session_id=p['session_id'])
    assert all(s['session_id'] != p['session_id'] for c in store.snapshot('family-a')['children'] for s in c['summaries'])
    assert store.delete('family-a', session_id=older['session_id'])
    assert not any(c.get('temporary') or c['summaries'] for c in store.snapshot('family-a')['children'])
    now[0] += 61*DAY; store.cleanup()
    with store.db() as db:
        assert db.execute('SELECT count(*) FROM summaries').fetchone()[0] == 0


def test_failed_requests_consume_durable_quota(tmp_path):
    http = HTTP(error=TimeoutError())
    for _ in range(3):
        with pytest.raises(ArkFailure): adapter(tmp_path, http, daily_calls=1).summarize([])
    assert len(http.requests) == 1


def test_private_config_refuses_symlink_bad_mode_and_empty_fields(tmp_path, monkeypatch):
    from types import SimpleNamespace
    from server import ark_summary as ark
    p = tmp_path/'ark.env'
    p.write_text('ARK_API_KEY=synthetic-only-key\nARK_MODEL=synthetic-model\n')
    p.chmod(0o640)
    real_fstat = ark.os.fstat
    def pretend_root(fd):
        s = real_fstat(fd)
        return SimpleNamespace(st_mode=s.st_mode, st_uid=0)
    monkeypatch.setattr(ark.os, 'fstat', pretend_root)
    assert ark.credentials(p) == ('synthetic-only-key', 'synthetic-model')
    link = tmp_path/'link'; link.symlink_to(p)
    with pytest.raises(OSError): ark.credentials(link)
    p.chmod(0o644)
    with pytest.raises(ArkFailure): ark.credentials(p)
    p.chmod(0o640); p.write_text('ARK_API_KEY=\nARK_MODEL=\n')
    with pytest.raises(ArkFailure): ark.credentials(p)


@pytest.mark.parametrize('changes, expected', [
    ({'parent_recap': '聊'*61}, {'parent_recap': '聊'*60}),
    ({'new_words': 'cat, dog'}, {'new_words': ['cat', 'dog']}),
])
def test_normalized_summary_stays_ark(system, tmp_path, changes, expected):
    store, _, secret, _, _, _ = system
    p = payload(store)
    assert store.upload(store.device(secret), Upload.model_validate(p))[0] == 200
    http = HTTP()
    http.data['choices'][0]['message']['content'] = json.dumps(dict(RESULT, **changes))
    worker = Worker(store, adapter(tmp_path, http))
    assert worker.once() and not worker.once()
    card = next(s for c in store.snapshot('family-a')['children'] for s in c['summaries'])
    assert card['status'] == 'ark-v1'
    with store.db() as db:
        saved = store.unseal(db.execute('SELECT payload FROM summaries WHERE session_id=?', (p['session_id'],)).fetchone()[0])
    assert all(saved[k] == v for k, v in expected.items())


def test_normalization_limits_and_extra_keys(tmp_path):
    http = HTTP()
    limits = dict(topic=100, recap=500, resume_question=160,
                  parent_recap=60, parent_highlight=60, parent_help=60)
    result = {k: 'x'*(limit+1) for k, limit in limits.items()}
    result.update(new_words=['x'*33] + ['cat']*13, extra='ignored')
    http.data['choices'][0]['message']['content'] = json.dumps(result)
    normalized = adapter(tmp_path, http)._request('synthetic', 'synthetic-key', 'synthetic-model')
    assert all(len(normalized[k]) == limit for k, limit in limits.items())
    assert normalized['new_words'] == ['cat']*12 and 'extra' not in normalized
    http.data['choices'][0]['message']['content'] = json.dumps(dict(RESULT, new_words=' cat，dog、bird;fish；ant/bee\nowl , '))
    assert adapter(tmp_path, http)._request('synthetic', 'synthetic-key', 'synthetic-model')['new_words'] == [
        'cat', 'dog', 'bird', 'fish', 'ant', 'bee', 'owl']


def test_default_limits_fit_ten_minute_conversation_once(system, tmp_path, monkeypatch):
    monkeypatch.delenv('ARK_SUMMARY_INPUT_TOKENS', raising=False)
    monkeypatch.delenv('ARK_SUMMARY_OUTPUT_TOKENS', raising=False)
    a = ArkSummarizer.from_environment()
    http = HTTP()
    a.key, a.model, a.usage_db, a.opener = 'synthetic-key', 'synthetic-model', str(tmp_path/'usage.sqlite'), http
    assert (a.input_tokens, a.output_tokens) == (8192, 1024)
    store, _, secret, _, _, _ = system
    p = payload(store)
    p['started_at'] -= 600
    p['turns'] = [dict(seq=i, role='user' if i % 2 == 0 else 'assistant', text='cat dog '*25)
                  for i in range(20)]
    size = len(json.dumps(dict(previous=None, turns=[{k: t[k] for k in ('role', 'text')} for t in p['turns']])).encode())
    assert 4600 <= size <= 4900
    assert store.upload(store.device(secret), Upload.model_validate(p))[0] == 200
    assert Worker(store, a).once()
    assert len(http.requests) == 1
    assert json.loads(http.requests[0][0].data)['max_tokens'] == 1024
    assert next(s for c in store.snapshot('family-a')['children'] for s in c['summaries'])['status'] == 'ark-v1'
