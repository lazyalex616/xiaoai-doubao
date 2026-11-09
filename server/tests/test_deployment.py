import os

import pytest

from server.config import data_key
from server.store import Store


def test_registration_without_parent_password_and_password_reset(tmp_path):
    store = Store(tmp_path/'memory.sqlite', os.urandom(32))
    secret = store.provision('home', None, 'speaker', ['Amy','Ben'])
    assert store.device(secret)['household_id'] == 'home'
    assert store.login('home', '') is None
    assert store.login('home', '!disabled') is None
    store.set_parent_password('home', 'a-user-selected-long-password')
    token, _ = store.login('home', 'a-user-selected-long-password')
    assert store.parent(token)
    store.set_parent_password('home', 'a-different-user-selected-password')
    assert store.parent(token) is None
    assert store.login('home', 'a-user-selected-long-password') is None
    assert store.login('home', 'a-different-user-selected-password')
    with pytest.raises(ValueError):
        store.set_parent_password('missing', 'a-user-selected-long-password')
    with pytest.raises(ValueError):
        store.set_parent_password('home', 'short')


def test_separate_key_file_precedes_legacy_environment(tmp_path, monkeypatch):
    key = os.urandom(32)
    filename = tmp_path/'data.key'
    filename.write_text(key.hex()+'\n')
    monkeypatch.setenv('MEMORY_DATA_KEY_FILE', str(filename))
    monkeypatch.setenv('MEMORY_DATA_KEY', 'invalid')
    assert data_key() == key
    filename.write_text('00')
    with pytest.raises(RuntimeError):
        data_key()
