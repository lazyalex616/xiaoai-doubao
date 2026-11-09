import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import runpy
import sys
import types

import pytest
from fastapi.testclient import TestClient

from server.app import create_app
from server.manage import ask_password
from server.store import Store

ORIGIN = 'https://xiaoai.test'


def fixture(tmp_path):
    store = Store(tmp_path/'memory.sqlite', os.urandom(32))
    secret = store.provision('home', 'testpass', 'speaker', ['Amy','Ben'])
    now = [0.0]
    return store, secret, now, create_app(store, origin=ORIGIN, background=False, clock=lambda: now[0])


def login(app, source, password='wrong', household='home'):
    with TestClient(app, base_url=ORIGIN, client=(source, 1234)) as client:
        return client.post('/parent/login', headers={'Origin': ORIGIN},
                           json={'household': household, 'password': password})


def test_global_guard_success_does_not_reset_and_existing_sessions_survive(tmp_path):
    store, secret, now, app = fixture(tmp_path)
    auth = login(app, 'parent', 'testpass')
    assert auth.status_code == 200
    for n in range(19):
        assert login(app, f'ip-{n}').status_code == 401
    assert login(app, 'success', 'testpass').status_code == 200
    tripped = login(app, 'unknown', household='missing')
    assert tripped.status_code == 429 and tripped.headers['Retry-After'] == '900'
    assert '15分钟' in tripped.json()['detail']
    assert login(app, 'correct', 'testpass').status_code == 429
    # Authenticated parents and device traffic remain usable.
    with TestClient(app, base_url=ORIGIN) as client:
        client.cookies.update(auth.cookies)
        assert client.get('/parent/data').status_code == 200
        assert client.get('/device/snapshot', headers={'Authorization': 'Bearer '+secret}).status_code == 200
        assert 'Retry-After' in client.get('/app.js').text
    now[0] = 899
    assert login(app, 'late').headers['Retry-After'] == '1'
    now[0] = 900
    assert login(app, 'correct', 'testpass').status_code == 200
    assert login(app, 'fresh').status_code == 401


def test_rolling_hour_and_ip_limit_are_independent(tmp_path):
    _, _, now, app = fixture(tmp_path)
    for _ in range(5):
        assert login(app, 'same-ip').status_code == 401
    for _ in range(25):
        assert login(app, 'same-ip').json()['detail'] == 'rate_limited'
    for n in range(14):
        assert login(app, f'ip-{n}').status_code == 401
    # Exact one-hour boundary expires all earlier failures.
    now[0] = 3600
    assert login(app, 'new-ip').status_code == 401
    for n in range(18):
        assert login(app, f'new-{n}').status_code == 401
    assert login(app, 'twentieth').status_code == 429


def test_global_guard_serializes_concurrent_sources(tmp_path):
    _, _, _, app = fixture(tmp_path)
    with ThreadPoolExecutor(max_workers=8) as pool:
        codes = list(pool.map(lambda n: login(app, f'ip-{n}').status_code, range(30)))
    assert codes.count(401) == 19
    assert codes.count(429) == 11


def test_parent_remember_login_expiry_logout_and_password_reset(tmp_path):
    store, _, _, app = fixture(tmp_path)
    now = [1_800_000_000]
    store.now = lambda: now[0]
    with TestClient(app, base_url=ORIGIN) as client:
        auth = client.post('/parent/login', headers={'Origin': ORIGIN},
                           json={'household': 'home', 'password': 'testpass'})
        cookie = auth.headers['set-cookie'].lower()
        assert 'max-age=2592000' in cookie
        assert all(flag in cookie for flag in ('secure', 'httponly', 'samesite=strict'))
        token = auth.cookies['parent_session']
        csrf = {'Origin': ORIGIN, 'X-CSRF-Token': auth.json()['csrf']}
        now[0] += 30 * 86400 - 1
        assert client.get('/parent/data').status_code == 200
        assert client.post('/parent/logout').status_code == 403
        assert client.post('/parent/logout', headers={**csrf, 'Origin': 'https://wrong.invalid'}).status_code == 403
        assert client.post('/parent/logout', headers=csrf).status_code == 200
        assert store.parent(token) is None
        token, _ = store.login('home', 'testpass')
        now[0] += 30 * 86400
        assert store.parent(token) is None
        token, _ = store.login('home', 'testpass')
        store.set_parent_password('home', 'new-test-password')
        assert store.parent(token) is None
        assert store.login('home', 'testpass') is None


@pytest.mark.parametrize('length,accepted', [(7, False), (8, True), (256, True), (257, False)])
def test_password_bounds_in_cli_and_store(tmp_path, monkeypatch, length, accepted):
    password = 'a'*length
    monkeypatch.setattr(sys.stdin, 'isatty', lambda: True)
    monkeypatch.setattr('server.manage.getpass.getpass', lambda _: password)
    store = Store(tmp_path/'memory.sqlite', os.urandom(32))
    store.provision('home', None, 'speaker', ['Amy','Ben'])
    if accepted:
        assert ask_password(argparse.ArgumentParser()) == password
        store.set_parent_password('home', password)
        assert store.login('home', password)
    else:
        with pytest.raises(SystemExit):
            ask_password(argparse.ArgumentParser())
        with pytest.raises(ValueError):
            store.set_parent_password('home', password)


@pytest.mark.parametrize('length', [7, 8])
def test_backup_passphrase_bounds_without_real_server(tmp_path, monkeypatch, length):
    calls = []
    key = os.urandom(32).hex().encode()+b'\n'
    fake = types.ModuleType('p5b')
    fake.check_host = lambda: None
    fake.remote = lambda *a, **kw: calls.append('key-read') or key
    monkeypatch.setitem(sys.modules, 'p5b', fake)
    monkeypatch.setattr(sys.stdin, 'isatty', lambda: True)
    monkeypatch.setattr('getpass.getpass', lambda _: 'a'*length)
    monkeypatch.setattr(Path, 'home', lambda: tmp_path)
    script = Path(__file__).resolve().parents[1]/'deploy/backup-key.py'
    if length == 7:
        with pytest.raises(SystemExit):
            runpy.run_path(str(script))
        assert calls == []
    else:
        runpy.run_path(str(script))
        backup = tmp_path/'.config/xiaoai/memory-data-key.backup.enc'
        assert backup.stat().st_mode & 0o777 == 0o600
        assert key not in backup.read_bytes()
        assert calls == ['key-read']
