import datetime as dt
import os

from server import learning
from server.backup import backup, restore
from server.models import Upload
from server.store import Store
from server.worker import Worker
from test_memory import system, payload, parent
from test_parent_daily import timestamp


def add(store, secret, child, sid, start, end, words=('cats',), quote='I like cats.', status='ark-v1'):
    data = payload(store, sid=sid)
    data.update(child_id=child, started_at=timestamp(start), ended_at=timestamp(end),
                turns=[dict(seq=0, role='user', text='I like cats.'),
                       dict(seq=1, role='assistant', text='I love dinosaurs.')])
    assert store.upload(store.device(secret), Upload.model_validate(data))[0] == 200
    with store.db() as db:
        summary = dict(topic='Pets', recap='Talked about pets.', resume_question='Which pet?', new_words=list(words),
                       parent_recap='聊过喜欢的宠物。', parent_highlight=f'表达喜好：“{quote}”', parent_help='求助词义：“尾巴怎么说？”')
        db.execute('INSERT INTO summaries VALUES(?,?,?,?,?,?,?)',
                   (sid, 'family-a', child, 0, int(store.now()), store.seal(summary), status))
        learning.remember_words(db, child, summary, data['started_at'])
    return data


def test_learning_auth_isolation_no_records_and_read_only(system):
    store, client, secret, other, now, _ = system
    now[0] = timestamp('2026-10-25T21:00:00')
    children = store.snapshot('family-a')['children']; child = children[0]['id']
    base = f'/parent/children/{child}'
    for path in ('/parent/learning', base+'/learning', base+'/history'):
        assert client.get(path).status_code == 401
    parent(client)
    data = client.get('/parent/learning').json()
    assert len(data['children']) == 2 and all(c['latest_at'] is None for c in data['children'])
    with store.db() as db:
        before = list(db.iterdump())
    overview = client.get(base+'/learning').json()
    assert overview['latest'] is None and overview['week_days'] == overview['week_seconds'] == overview['streak'] == 0
    assert overview['words'] == [] and not overview['previous_has_records']
    assert client.get(base+'/history').json() == dict(days=[], next_before=None)
    foreign = store.snapshot('family-b')['children'][0]['id']
    assert client.get(f'/parent/children/{foreign}/learning').status_code == 404
    assert client.get(f'/parent/children/{foreign}/history').status_code == 404
    assert client.get(base+'/history?before=bad').status_code == 422
    with store.db() as db:
        assert list(db.iterdump()) == before
    add(store, secret, child, 'only-one', '2026-10-25T19:00:00', '2026-10-25T19:07:00')
    latest = client.get('/parent/learning').json()['children']
    assert sum(c['latest_at'] is not None for c in latest) == 1
    empty = next(c['id'] for c in children if c['id'] != child)
    assert client.get(f'/parent/children/{empty}/learning').json()['latest'] is None
    # New reads use the existing account limiter and never grant write privileges.
    assert client.delete('/parent/sessions/only-one').status_code == 403
    for _ in range(120):
        response = client.get(base+'/learning')
        if response.status_code == 429:
            break
    assert response.status_code == 429


def test_midnight_same_weekday_comparison_and_day_pagination(system):
    store, client, secret, _, now, _ = system
    now[0] = timestamp('2026-10-28T21:00:00')  # Wednesday, UTC remains the same day.
    child = payload(store)['child_id']
    add(store, secret, child, 'last-mon', '2026-10-19T12:00:00', '2026-10-19T12:04:00')
    add(store, secret, child, 'last-wed', '2026-10-21T12:00:00', '2026-10-21T12:03:00')
    add(store, secret, child, 'last-thu', '2026-10-22T12:00:00', '2026-10-22T12:59:00')
    add(store, secret, child, 'overnight', '2026-10-25T23:58:00', '2026-10-26T00:03:00')
    add(store, secret, child, 'tue', '2026-10-27T12:00:00', '2026-10-27T12:02:00')
    add(store, secret, child, 'midnight-end', '2026-10-27T23:59:00', '2026-10-28T00:00:00')
    add(store, secret, child, 'now-1', '2026-10-28T12:00:00', '2026-10-28T12:05:00')
    add(store, secret, child, 'now-2', '2026-10-28T19:00:00', '2026-10-28T19:07:00')
    add(store, secret, child, 'old', '2026-10-01T12:00:00', '2026-10-01T12:01:00')
    parent(client)
    base = f'/parent/children/{child}'
    data = client.get(base+'/learning').json()
    assert data['week_days'] == 3 and data['streak'] == 4
    assert data['week_seconds'] == 18*60 and data['previous_seconds'] == 7*60
    assert data['previous_has_records'] and data['today_count'] == 2 and data['today_seconds'] == 12*60
    assert [day['seconds'] for day in data['week']] == [180, 180, 720, 0, 0, 0, 0]
    first = client.get(base+'/history').json()
    assert len(first['days']) == 7 and first['next_before'] == '2026-10-19'
    second = client.get(base+'/history?before='+first['next_before']).json()
    assert [day['date'] for day in second['days']] == ['2026-10-01'] and second['next_before'] is None
    assert first['days'][0]['seconds'] == 720
    assert all(r['session_id'] != 'midnight-end' for r in first['days'][0]['records'])
    # On a new Shanghai day with no record, the latest session and streak remain truthful.
    now[0] = timestamp('2026-10-29T00:30:00')
    data = client.get(base+'/learning').json()
    assert data['date'] == '2026-10-29' and data['today_count'] == 0 and data['streak'] == 4


def test_first_words_backfill_worker_deletion_and_restore(system, tmp_path):
    store, client, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T21:00:00'); child = payload(store)['child_id']
    add(store, secret, child, 'before', '2026-10-18T12:00:00', '2026-10-18T12:01:00', ('Cats', 'my school', '[private]'))
    add(store, secret, child, 'week', '2026-10-24T12:00:00', '2026-10-24T12:01:00', ('cats', 'Wings'))
    with store.db() as db:
        db.execute('DROP TABLE first_words')  # Emulate the main baseline database.
    learning.migrate_words(store)
    learning.migrate_words(store)  # One-time and idempotent.
    assert learning.overview(store, 'family-a', child)['words'] == [dict(word='cats', first=False), dict(word='wings', first=True)]
    with store.db() as db:
        assert [r['name'] for r in db.execute('PRAGMA table_info(first_words)')] == ['child_id', 'word', 'first_date']
    # Serial worker updates first dates inside its summary transaction.
    data = payload(store, sid='worker'); data.update(started_at=timestamp('2026-10-25T18:00:00'), ended_at=timestamp('2026-10-25T18:02:00'))
    store.upload(store.device(secret), Upload.model_validate(data))
    with store.db() as db:
        db.execute("DELETE FROM jobs WHERE session_id != 'worker'")
    assert Worker(store).once()
    with store.db() as db:
        assert db.execute('SELECT first_date FROM first_words WHERE child_id=? AND word=?', (child, 'dinosaurs')).fetchone()[0] == '2026-10-25'
    auth = parent(client)
    assert client.delete('/parent/sessions/before', headers=auth).status_code == 200
    assert learning.overview(store, 'family-a', child)['words'][0] == dict(word='cats', first=False)
    # Use a separately keyed legacy fixture to exercise an actual restore, then migration.
    key = os.urandom(32); legacy = Store(tmp_path/'legacy.sqlite', key, now=lambda:now[0])
    legacy_secret = legacy.provision('family-a', 'a-long-parent-password', 'speaker-a', ['Amy','Ben'])
    legacy_child = payload(legacy)['child_id']
    add(legacy, legacy_secret, legacy_child, 'legacy', '2026-10-24T12:00:00', '2026-10-24T12:01:00')
    with legacy.db() as db: db.execute('DROP TABLE first_words')
    artifact = backup(legacy, tmp_path/'legacy-backup')
    restored = restore(artifact, tmp_path/'restored.sqlite', key, tmp_path/'legacy-backup/latest.deletions')
    with restored.db() as db:
        assert db.execute('SELECT word,first_date FROM first_words').fetchone()[:] == ('cats', '2026-10-24')


def test_only_complete_child_english_and_retention(system):
    store, _, secret, _, now, _ = system
    now[0] = timestamp('2026-10-25T21:00:00'); child = payload(store)['child_id']
    add(store, secret, child, 'child', '2026-10-24T12:00:00', '2026-10-24T12:01:00')
    add(store, secret, child, 'assistant', '2026-10-25T12:00:00', '2026-10-25T12:01:00', quote='I love dinosaurs.')
    data = learning.overview(store, 'family-a', child)
    assert data['latest']['child_english'] == [] and data['expression']['session_id'] == 'child'
    ben = next(c['id'] for c in store.snapshot('family-a')['children'] if c['id'] != child)
    add(store, secret, ben, 'help-only', '2026-10-25T12:00:00', '2026-10-25T12:01:00', quote='I love dinosaurs.')
    help_only = learning.overview(store, 'family-a', ben)['expression']
    assert help_only['child_english'] == [] and help_only['summary']['parent_help'] != '未观察到'
    add(store, secret, child, 'failed', '2026-10-25T19:00:00', '2026-10-25T19:01:00', status='fallback')
    assert learning.overview(store, 'family-a', child)['latest']['status'] == 'fallback'
    now[0] += 31*86400
    records = learning.sessions(store, 'family-a', child)
    assert all(r['transcript'] is None and r['child_english'] == [] for r in records)
    now[0] += 60*86400
    assert all(r['summary'] is None for r in learning.sessions(store, 'family-a', child))
