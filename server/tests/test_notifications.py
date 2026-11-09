import datetime as dt
import io
import json
import urllib.error
from zoneinfo import ZoneInfo

import pytest

from server.models import Upload
from server.notifications import DailyNotifier, WecomSender, digest
from test_memory import ORIGIN, headers, parent, payload, system


@pytest.fixture(autouse=True)
def parent_link(monkeypatch):
    monkeypatch.setenv('PARENT_URL', 'https://parent.example.org')


def timestamp(value):
    return int(dt.datetime.fromisoformat(value).replace(tzinfo=ZoneInfo('Asia/Shanghai')).timestamp())


class Sender:
    def __init__(self, *results):
        self.results = list(results) or ['sent']
        self.messages = []

    def send(self, content):
        self.messages.append(content)
        return self.results.pop(0) if len(self.results) > 1 else self.results[0]


def practice(store, secret, now, sid='practice', name='Amy', **summary):
    child = next(c for c in store.snapshot('family-a')['children'] if c['profile']['name'] == name)
    data = dict(payload(store, sid=sid), child_id=child['id'], started_at=now-300, ended_at=now)
    assert store.upload(store.device(secret), Upload.model_validate(data))[0] == 200
    value = dict(topic='动物', recap='do not send recap', resume_question='do not send question',
                 parent_recap='聊过动物和食物', parent_highlight='能表达喜好：“I like dinosaurs.”',
                 parent_help='求助词义：“尾巴怎么说？”', new_words=['tail', 'apple'])
    value.update(summary)
    with store.db() as db:
        db.execute('INSERT INTO summaries VALUES(?,?,?,?,?,?,?)',
                   (sid, 'family-a', child['id'], 0, now, store.seal(value), 'ark-v1'))
    return data


def test_digest_multichild_no_transcript_and_deletion(system):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T19:00:00')
    practice(store, secret, now[0], new_words=['tail'], parent_recap='聊过动物和食物。I like dinosaurs. Which dinosaur?')
    practice(store, secret, now[0], sid='ben', name='Ben', topic='交通', parent_recap='聊过交通工具', new_words=['bus'])
    day = dt.date(2026, 10, 25)
    content = digest(store, 'family-a', day)
    assert all(v in content for v in ['Amy', 'Ben', '5 分钟', 'tail', 'bus'])
    assert all(v not in content for v in ['I like dinosaurs.', '尾巴怎么说', 'Which dinosaur?', 'do not send', 'transcript'])
    assert digest(store, 'family-b', day) is None
    store.delete('family-a', session_id='practice')
    content = digest(store, 'family-a', day)
    assert 'tail' not in content and '动物' not in content and '今天没有记录' in content
    store.delete('family-a', session_id='ben')
    assert digest(store, 'family-a', day) is None


@pytest.mark.parametrize('observation,words', [
    ('出现人身伤害：被打了', ['I have asthma']),
    ('提到哮喘', ['tail', 'apple', 'I have asthma']),
    ('孩子说过一句完整英文', ['ice-cream', "can't", 'abc-def-ghi', 'a'*21, 'hello world', 'café', 'word!', 'tail']),
])
def test_digest_uses_only_single_word_whitelist(system, observation, words):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T20:00:00')
    practice(store, secret, now[0], topic='哮喘：被打了', parent_recap=observation,
             parent_highlight=observation, parent_help=observation, new_words=words)
    content = digest(store, 'family-a', dt.date(2026, 10, 25))
    assert content.endswith('详情：https://parent.example.org')
    assert all(value not in content for value in ['学习类别', '详细小结', '聊过不代表学会', '内容较多'])
    assert all(value not in content for value in [observation, '被打了', '哮喘', 'I have asthma',
                                                'abc-def-ghi', 'a'*21, 'hello world', 'café', 'word!'])
    assert all(value in content for value in words if value in ['tail', 'apple', 'ice-cream', "can't"])


@pytest.mark.parametrize('seconds,expected', [(0, '不到 1 分钟'), (59, '不到 1 分钟'),
                                               (60, '1 分钟'), (90, '2 分钟'), (150, '3 分钟')])
def test_digest_duration_empty_child_and_word_limit(system, seconds, expected):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T20:00:00')
    words = ['apple', 'bus', 'cat', 'dog', 'egg', 'fish', 'goat', 'hat', 'ice', 'jar', 'kite', 'lion', 'moon']
    practice(store, secret, now[0], new_words=words)
    with store.db() as db:
        db.execute('UPDATE sessions SET started_at=? WHERE id=?', (now[0]-seconds, 'practice'))
    content = digest(store, 'family-a', dt.date(2026, 10, 25))
    assert set(content.splitlines()) == {'## 2026-10-25 英语练习小结', f'### Amy · {expected}',
                                         '聊过的词：'+'、'.join(words[:12]), '### Ben · 不到 1 分钟',
                                         '今天没有记录', '详情：https://parent.example.org'}
    assert len(content.splitlines()) == 6 and content.endswith('详情：https://parent.example.org')


@pytest.mark.parametrize('words', [[], ['I have asthma', 'café']])
def test_digest_hides_empty_word_line(system, words):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T20:00:00')
    practice(store, secret, now[0], new_words=words)
    content = digest(store, 'family-a', dt.date(2026, 10, 25))
    assert all(value not in content for value in ['聊过的词', '暂无记录'])
    assert '### Amy · 5 分钟' in content and content.endswith('详情：https://parent.example.org')


def test_schedule_shanghai_persistence_empty_disable_and_custom_time(system):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T19:59:59')
    practice(store, secret, now[0])
    sender = Sender()
    notifier = DailyNotifier(store, sender, 'family-a')
    assert not notifier.once()
    now[0] += 1
    assert notifier.once() and len(sender.messages) == 1
    notifier.save_settings('family-a', True, '21:30')
    now[0] += 6000
    assert not DailyNotifier(store, sender, 'family-a').once()  # restart + time edit cannot duplicate
    now[0] = timestamp('2026-10-26T21:30:00')
    assert not notifier.once() and len(sender.messages) == 1  # empty day: skip
    now[0] = timestamp('2026-10-27T21:29:59')
    practice(store, secret, now[0], sid='day3')
    assert not notifier.once()
    now[0] += 1
    notifier.save_settings('family-a', False, '21:30')
    assert not notifier.once()
    notifier.save_settings('family-a', True, '21:30')
    assert notifier.once() and len(sender.messages) == 2
    assert DailyNotifier(store, sender, 'family-a').settings('family-a')['time'] == '21:30'


@pytest.mark.parametrize('result', ['retry', 'uncertain', 'failed'])
def test_bounded_retry_terminal_unknown_and_restart(system, caplog, result):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T20:00:00')
    practice(store, secret, now[0])
    sender = Sender(result)
    notifier = DailyNotifier(store, sender, 'family-a')
    assert not notifier.once()
    assert not notifier.once()
    now[0] += 59
    assert not notifier.once() and len(sender.messages) == 1
    for seconds in [1, 300, 600]:
        now[0] += seconds
        assert not DailyNotifier(store, sender, 'family-a').once()
    assert len(sender.messages) == (3 if result == 'retry' else 1)
    assert 'wecom_daily state=' in caplog.text
    assert all(v not in caplog.text for v in ['Amy', '动物', 'tail', 'http', 'I like'])
    with store.db() as db:
        db.execute("UPDATE notification_days SET state='sending'")
    assert not notifier.once()  # interrupted request is never replayed


def test_retry_reads_latest_deletions(system):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T20:00:00')
    practice(store, secret, now[0])
    sender = Sender('retry', 'sent')
    notifier = DailyNotifier(store, sender, 'family-a')
    assert not notifier.once()
    store.delete('family-a', session_id='practice')
    now[0] += 60
    assert not notifier.once() and len(sender.messages) == 1


def test_settings_auth_csrf_isolation_profile_limits_and_device_sync(system):
    store, client, secret, other, _, _ = system
    assert client.get('/parent/notifications').status_code == 401
    assert client.get('/parent/notifications', headers=headers(secret)).status_code == 401
    auth = parent(client)
    assert client.put('/parent/notifications', json={'enabled': True, 'time': '21:15'}).status_code == 403
    assert client.put('/parent/notifications', headers=auth, json={'enabled': True, 'time': '24:00'}).status_code == 422
    assert client.put('/parent/notifications', headers=auth, json={'enabled': True, 'time': '21:15', 'webhook': 'secret'}).status_code == 422
    settings = client.put('/parent/notifications', headers=auth, json={'enabled': True, 'time': '21:15'}).json()
    assert settings == dict(enabled=True, time='21:15', ready=False, timezone='Asia/Shanghai')
    child = store.snapshot('family-a')['children'][0]
    profile = dict(child['profile'], grade='二年级', level='日常对话', interests='汉'*12, goals='恐龙和食物')
    for field in ['grade', 'level', 'interests', 'goals']:
        invalid = dict(profile, **{field: '汉'*13})
        assert client.put('/parent/profiles/'+child['id'], headers=auth, json=invalid).status_code == 422
    assert client.put('/parent/profiles/'+child['id'], headers=auth, json=profile).status_code == 200
    synced = client.get('/device/snapshot', headers=headers(secret)).json()['children']
    updated = next(c for c in synced if c['id'] == child['id'])
    assert updated['profile'] == profile and updated['version'] == child['version']+1
    assert child['id'] not in {c['id'] for c in client.get('/device/snapshot', headers=headers(other)).json()['children']}
    parent(client, 'family-b', 'another-parent-password')
    assert client.get('/parent/notifications').json()['time'] == '20:00'


def test_private_config_sender_payload_error_redaction_and_limit(system, tmp_path, monkeypatch, caplog):
    fake = 'https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=FAKE_TEST_ONLY'
    path = tmp_path/'webhook.txt'
    path.write_text(fake+'\n'); path.chmod(0o600)
    monkeypatch.setenv('WECOM_WEBHOOK_FILE', str(path)); monkeypatch.setenv('WECOM_HOUSEHOLD', 'family-a')
    assert WecomSender.from_environment() is not None
    path.chmod(0o644)
    with pytest.raises(RuntimeError, match='invalid private WeCom configuration'):
        WecomSender.from_environment()
    path.chmod(0o600)
    class HTTP:
        result = {'errcode': 0}
        request = None
        def open(self, request, timeout):
            self.request = request
            assert timeout == 5
            if isinstance(self.result, Exception): raise self.result
            return io.BytesIO(json.dumps(self.result).encode())
    http = HTTP(); sender = WecomSender(fake, http)
    assert sender.send('虚构中文小结') == 'sent'
    assert json.loads(http.request.data) == {'msgtype': 'markdown', 'markdown': {'content': '虚构中文小结'}}
    for result, state in [({'errcode': -1}, 'retry'), ({'errcode': 45009}, 'retry'),
                          ({'errcode': 40014, 'errmsg': fake}, 'failed'), (TimeoutError(fake), 'uncertain'),
                          (urllib.error.HTTPError(fake, 302, 'redirect', {}, None), 'uncertain')]:
        http.result = result
        assert sender.send('虚构中文小结') == state
    assert fake not in caplog.text
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T20:00:00')
    for i in range(20): practice(store, secret, now[0], sid=f's{i}', parent_recap='汉'*60, new_words=['汉'*32]*12)
    assert len(digest(store, 'family-a', dt.date(2026,10,25)).encode()) <= 4096


def test_no_parent_link(system, monkeypatch):
    store, _, secret, _, _, _ = system
    now = timestamp('2026-10-26T18:00:00')
    practice(store, secret, now)
    monkeypatch.delenv('PARENT_URL')
    assert '详情：' not in digest(store, 'family-a', dt.date(2026, 10, 26))
