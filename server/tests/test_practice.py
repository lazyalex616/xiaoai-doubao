from server.models import Upload
from test_memory import system, payload, parent
from test_parent_daily import timestamp


def test_practice_login_bounds_and_shanghai_calendar(system):
    store, client, _, _, now, _ = system
    now[0] = timestamp('2026-10-26T00:30:00')  # Still October 25 in UTC.
    assert client.get('/parent/practice').status_code == 401
    parent(client)
    for days in ('0', '61', '-1', '1.5', 'bad'):
        assert client.get('/parent/practice?days='+days).status_code == 422
    data = client.get('/parent/practice').json()
    assert data['date'] == '2026-10-26' and data['timezone'] == 'Asia/Shanghai'
    assert len(data['days']) == 60 and data['days'][-1]['date'] == data['date']
    assert all(value == 0 for day in data['days'] for value in day['minutes'].values())
    assert len(client.get('/parent/practice?days=1').json()['days']) == 1


def test_practice_overlap_totals_isolation_deletion_and_read_only(system):
    store, client, secret, other_secret, now, _ = system
    now[0] = timestamp('2026-10-26T00:30:00')
    children = {c['profile']['name']: c['id'] for c in store.snapshot('family-a')['children']}
    def add(sid, child, start, end, household='family-a', token=secret):
        p = payload(store, household, sid)
        p.update(child_id=child, started_at=timestamp(start), ended_at=timestamp(end))
        assert store.upload(store.device(token), Upload.model_validate(p))[0] == 200
    add('overnight', children['Amy'], '2026-10-24T23:58:00', '2026-10-25T00:03:00')
    add('second', children['Amy'], '2026-10-25T08:00:00', '2026-10-25T08:01:30')
    add('midnight-end', children['Ben'], '2026-10-25T23:59:30', '2026-10-26T00:00:00')
    add('today', children['Ben'], '2026-10-26T00:01:00', '2026-10-26T00:01:01')
    add('zero', children['Amy'], '2026-10-26T00:02:00', '2026-10-26T00:02:00')
    add('multiday', children['Amy'], '2026-10-23T23:59:00', '2026-10-25T00:01:00')
    foreign = store.snapshot('family-b')['children'][0]['id']
    add('foreign', foreign, '2026-10-25T12:00:00', '2026-10-25T12:10:00', 'family-b', other_secret)
    auth = parent(client)
    with store.db() as db:
        before = list(db.iterdump())
    response = client.get('/parent/practice?days=4')
    assert response.status_code == 200
    with store.db() as db:
        assert list(db.iterdump()) == before  # GET does not cleanup or write any tables.
    days = {d['date']: d['minutes'] for d in response.json()['days']}
    assert days['2026-10-23'] == {children['Amy']: 1, children['Ben']: 0}
    assert days['2026-10-24'] == {children['Amy']: 1442, children['Ben']: 0}
    assert days['2026-10-25'] == {children['Amy']: 5.5, children['Ben']: .5}
    assert days['2026-10-26'] == {children['Amy']: 0, children['Ben']: 1/60}
    assert foreign not in response.text and 'transcript' not in response.text
    assert client.delete('/parent/sessions/overnight', headers=auth).status_code == 200
    days = client.get('/parent/practice?days=3').json()['days']
    assert days[0]['minutes'][children['Amy']] == 1440
    assert days[1]['minutes'][children['Amy']] == 2.5
    assert client.get('/parent/data?date=2026-10-25').json()['records']
    parent(client, 'family-b', 'another-parent-password')
    foreign_days = client.get('/parent/practice?days=2').json()['days']
    assert foreign_days[0]['minutes'][foreign] == 10
    assert all(set(day['minutes']).isdisjoint(children.values()) for day in foreign_days)
