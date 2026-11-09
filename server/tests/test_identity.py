import json
import sqlite3
import time
from concurrent.futures import ThreadPoolExecutor
from fastapi.testclient import TestClient
from server.app import create_app
from server.ark_identity import ArkIdentity, prompt
from test_ark_summary import HTTP, adapter
from test_memory import system
import pytest


def http_result(value):
    return HTTP(data={'choices': [{'finish_reason': 'stop', 'message': {'content': json.dumps(value)}}],
                     'usage': {'prompt_tokens': 200, 'completion_tokens': 8}})


def test_one_sentence_only_and_shared_cap(tmp_path, caplog):
    h = http_result({'result': 'adult'})
    b = adapter(tmp_path, h, daily_calls=1)
    assert ArkIdentity(b).classify('opaque', ['Amy','Ben']) == 'adult'
    request, timeout = h.requests[0]
    payload = json.loads(request.data)
    assert payload['messages'] == [{'role': 'system', 'content': prompt(['Amy','Ben'])}, {'role': 'user', 'content': 'opaque'}]
    assert payload['max_tokens'] == 32 and timeout < 2
    assert 'Emmy' not in caplog.text and 'synthetic-private-key' not in caplog.text
    with pytest.raises(Exception):
        b.summarize([])
    assert ArkIdentity(adapter(tmp_path, h, daily_calls=1)).classify('opaque', ['Amy','Ben']) == 'unknown'
    assert len(h.requests) == 1
    with sqlite3.connect(b.usage_db) as db:
        assert db.execute('SELECT calls,input_tokens,output_tokens FROM usage').fetchone() == (1, 200, 8)


@pytest.mark.parametrize('value', [{'result': 'evil'}, {'result': 'amy', 'text': 'private'}, {}, [], {'result': 1}])
def test_strict_result_no_retry(tmp_path, value):
    h = http_result(value)
    assert ArkIdentity(adapter(tmp_path, h)).classify('opaque', ['Amy','Ben']) == 'unknown'
    assert len(h.requests) == 1


def test_failed_call_consumes_existing_cap(tmp_path):
    h = HTTP(error=TimeoutError('private provider text'))
    b = adapter(tmp_path, h, daily_calls=1)
    assert ArkIdentity(b).classify('opaque', ['Amy','Ben']) == 'unknown'
    assert ArkIdentity(b).classify('opaque', ['Amy','Ben']) == 'unknown'
    assert len(h.requests) == 1


def test_endpoint_device_auth_extra_fields_and_deadline(system):
    store, _, secret, _, _, _ = system
    class Classifier:
        def __init__(self): self.calls = 0
        def classify(self, text, names):
            self.calls += 1
            if text == 'slow': time.sleep(2.3)
            return 'ben'
    c = Classifier()
    with TestClient(create_app(store, background=False, classifier=c)) as client:
        auth = {'authorization': 'Bearer '+secret}
        assert client.post('/device/identity', json={'text': 'Ben'}).status_code == 401
        assert client.post('/device/identity', headers=auth, json={'text': 'Ben', 'history': 'forbidden'}).status_code == 422
        assert client.post('/device/identity', headers=auth, json={'text': 'x'*161}).status_code == 422
        assert c.calls == 0
        assert client.post('/device/identity', headers=auth, json={'text': 'opaque'}).json() == {'result': 'ben'}
        start = time.monotonic()
        assert client.post('/device/identity', headers=auth, json={'text': 'slow'}).json() == {'result': 'unknown'}
        assert time.monotonic()-start < 2.1
        # No queue or second paid request while timed-out provider still runs.
        assert client.post('/device/identity', headers=auth, json={'text': 'opaque'}).json() == {'result': 'unknown'}
        assert c.calls == 2


@pytest.mark.parametrize('text', ['我是 Emmy', '我是 Aimee', 'I am Emmy', '我是艾米', '不是 Amy', 'Amy 说我是 Ben', '我是 Ben 的同学', '"Amy"'])
def test_never_grants_guessed_child(tmp_path, text):
    h = http_result({'result': 'amy'})
    assert ArkIdentity(adapter(tmp_path, h)).classify(text, ['Amy','Ben']) == 'unknown'


@pytest.mark.parametrize('text, result', [('Amy', 'amy'), ('I am Ben', 'ben'), ('我是 Amy', 'amy')])
def test_exact_name_without_model(tmp_path, text, result):
    h = http_result({'result': 'unknown'})
    assert ArkIdentity(adapter(tmp_path, h)).classify(text, ['Amy','Ben']) == result
    assert not h.requests


@pytest.mark.parametrize('names,labels,rule', [([], 'adult|exit|unknown', None),
    (['Amy'], 'amy|adult|exit|unknown', 'Only the exact name Amy may identify a child'),
    (['Amy','Ben','Cat'], 'amy|ben|cat|adult|exit|unknown', 'Only the exact names Amy, Ben and Cat may identify a child')])
def test_family_prompt(names, labels, rule):
    from server.ark_identity import allowed, exact_child
    text = prompt(names)
    assert labels+'.' in text
    assert (rule in text) if rule else 'Only the exact name' not in text
    assert allowed(names) == set(labels.split('|'))
    assert exact_child('Amy', names) == ('amy' if names else None)
