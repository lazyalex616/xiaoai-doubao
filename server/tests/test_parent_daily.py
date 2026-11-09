import datetime
import json
from zoneinfo import ZoneInfo

import pytest

from server.models import Upload
from server.store import DAY
from server.worker import Worker
from test_ark_summary import HTTP, Reply, RESULT, adapter
from test_memory import system, payload, parent


def timestamp(value):
    return int(datetime.datetime.fromisoformat(value).replace(tzinfo=ZoneInfo('Asia/Shanghai')).timestamp())


def test_chinese_observations_are_grounded_and_survive_later_chunks(tmp_path, caplog):
    observations = dict(parent_recap='聊过恐龙和食物。', parent_highlight='表达喜好：“I like dinosaurs.”',
                        parent_help='求助词义：“尾巴怎么说？”')
    class ChunkHTTP(HTTP):
        def open(self, request, timeout):
            self.requests.append((request, timeout))
            previous = json.loads(json.loads(request.data)['messages'][1]['content'])['previous']
            value = dict(RESULT, **observations) if previous is None else dict(RESULT, parent_recap='未观察到')
            return Reply(json.dumps(dict(choices=[dict(finish_reason='stop', message=dict(content=json.dumps(value)))],
                                         usage=dict(prompt_tokens=200, completion_tokens=100))).encode())
    http = ChunkHTTP()
    turns = [dict(role='user', text='I like dinosaurs. 尾巴怎么说？' + '恐龙吃植物。'*400),
             dict(role='user', text='Bye.')]
    result = adapter(tmp_path, http).summarize(turns)
    assert len(http.requests) > 2
    assert all(result[k] == v for k, v in observations.items())
    for request, _ in http.requests[1:]:
        previous = json.loads(json.loads(request.data)['messages'][1]['content'])['previous']
        assert all(previous[k] == v for k, v in observations.items())
    assert '恐龙' not in caplog.text and 'I like dinosaurs' not in caplog.text


@pytest.mark.parametrize('turns', [[], [dict(role='assistant', text='I like dinosaurs.')],
                                  [dict(role='user', text='Bye.')]])
def test_absent_user_evidence_does_not_invent_highlight_or_help(tmp_path, turns):
    http = HTTP()
    value = dict(RESULT, parent_highlight='表达喜好：“I like dinosaurs.”', parent_help='已掌握英语')
    http.data['choices'][0]['message']['content'] = json.dumps(value)
    result = adapter(tmp_path, http).summarize(turns)
    assert result['parent_highlight'] == result['parent_help'] == '未观察到'
    if not any(t['role'] == 'user' for t in turns):
        assert result['parent_recap'] == '未观察到'


def test_parent_fields_stored_but_device_card_unchanged(system, tmp_path):
    store, client, secret, _, _, _ = system
    p = payload(store)
    store.upload(store.device(secret), Upload.model_validate(p))
    assert Worker(store, adapter(tmp_path)).once()
    parent(client)
    record = client.get('/parent/data').json()['records'][0]
    assert record['summary'] == RESULT and record['ended_at'] == p['ended_at']
    card = next(c['summaries'][0] for c in store.snapshot('family-a')['children'] if c['summaries'])
    assert set(card) == {'topic', 'recap', 'resume_question', 'new_words', 'status', 'session_id', 'date'}
    assert all(card[k] == RESULT[k] for k in ('topic', 'recap', 'resume_question', 'new_words'))


def test_shanghai_day_all_sessions_retention_and_delete(system):
    store, client, secret, _, now, worker = system
    now[0] = timestamp('2026-10-26T00:30:00')
    amy = payload(store, sid='overnight')
    amy.update(started_at=timestamp('2026-10-24T23:58:00'), ended_at=timestamp('2026-10-25T00:03:00'))
    store.upload(store.device(secret), Upload.model_validate(amy)); assert worker.once()
    ben = next(c['id'] for c in store.snapshot('family-a')['children'] if c['profile']['name'] == 'Ben')
    for i in range(201):
        p = dict(amy, session_id=f'ben-{i}', child_id=ben,
                 started_at=timestamp('2026-10-25T08:00:00'), ended_at=timestamp('2026-10-25T08:01:30'))
        assert store.upload(store.device(secret), Upload.model_validate(p))[0] == 200
    auth = parent(client)
    assert client.get('/parent/data').json()['date'] == '2026-10-26'  # UTC is still the 25th.
    data = client.get('/parent/data?date=2026-10-25').json()
    assert data['date'] == '2026-10-25' and len(data['records']) == 202
    assert len(client.get('/parent/data?date=2026-10-24').json()['records']) == 1
    assert client.get('/parent/data?date=2026-10-26').json()['records'] == []
    assert client.get('/parent/data?date=bad').status_code == 422
    now[0] += 31*DAY
    assert client.delete('/parent/sessions/overnight', headers=auth).status_code == 401
    auth = parent(client)
    data = client.get('/parent/data?date=2026-10-25').json()
    assert all(r['transcript'] is None for r in data['records'])
    record = next(r for r in data['records'] if r['session_id'] == 'overnight')
    assert record['ended_at'] - record['started_at'] == 300 and record['summary']
    assert client.delete('/parent/sessions/overnight', headers=auth).status_code == 200
    data = client.get('/parent/data?date=2026-10-25').json()
    assert len(data['records']) == 201 and all(r['child_id'] == ben for r in data['records'])
    assert client.get('/parent/data?date=2026-10-24').json()['records'] == []
    with store.db() as db:
        assert db.execute('SELECT count(*) FROM summaries WHERE session_id="overnight"').fetchone()[0] == 0
    now[0] += 60*DAY
    store.cleanup()
    assert all(r['summary'] is None for r in store.records('family-a', datetime.date(2026, 10, 25))['records'])
