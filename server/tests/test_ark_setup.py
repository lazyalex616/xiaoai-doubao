import io
import json
import os
import stat
from types import SimpleNamespace
import urllib.error
import warnings

import pytest

from server import ark_setup as ark

# Synthetic fixtures only; no real credentials/network.
KEY = 'synthetic-key-for-tests'


class Response(io.BytesIO):
    status = 200


def fake_http(monkeypatch, *, status=200, code=None, raw=None):
    calls = []
    payload = raw if raw is not None else json.dumps(
        {'choices': [{'message': {'content': 'ok'}}]}).encode()

    def open_once(request, timeout):
        calls.append((request, timeout))
        if status != 200:
            raise urllib.error.HTTPError(request.full_url, status, KEY, {}, io.BytesIO(
                json.dumps({'error': {'code': code, 'message': KEY}}).encode()))
        return Response(payload)

    monkeypatch.setattr(ark.urllib.request, 'build_opener',
                        lambda *handlers: SimpleNamespace(open=open_once))
    return calls


def test_single_tiny_request(monkeypatch):
    calls = fake_http(monkeypatch)
    assert ark.validate_once(KEY, ark.DEFAULT_MODEL) is None
    assert len(calls) == 1
    request, timeout = calls[0]
    body = json.loads(request.data)
    assert request.full_url == ark.ARK_URL
    assert body['model'] == 'doubao-seed-2-0-mini-260428'
    assert body['max_tokens'] == 8 and body['thinking'] == {'type': 'disabled'}
    assert body['messages'] == [{'role': 'user', 'content': 'Reply only ok.'}]
    assert request.get_header('Authorization') == 'Bearer '+KEY and timeout == 15


@pytest.mark.parametrize('status,code,expected', [
    (401, 'AuthenticationError', '鉴权失败'),
    (404, 'ModelNotOpen', '模型未开通'),
    (403, 'AccountOverdueError', '余额不足'),
    (403, 'OperationDenied.ServiceOverdue', '余额不足'),
    (404, 'InvalidEndpointOrModel.ModelIDAccessDisabled', '自建接入点'),
    (404, 'InvalidEndpointOrModel.NotFound', '不存在'),
    (403, 'AccessDenied', '无访问权限'),
    (429, 'RateLimitExceeded', '限流'),
    (500, KEY, '暂时不可用'),
    (400, KEY, '请求被拒绝'),
    (302, KEY, '请求被拒绝'),
])
def test_failure_classification_never_echoes_remote(monkeypatch, status, code, expected):
    calls = fake_http(monkeypatch, status=status, code=code)
    reason = ark.validate_once(KEY, ark.DEFAULT_MODEL)
    assert expected in reason and KEY not in reason
    assert len(calls) == 1


@pytest.mark.parametrize('raw', [b'bad json', b'{}', b'{"choices": []}',
                                  b'{"choices":[{"message":{"content":""}}]}', b'x'*16385])
def test_malformed_success_is_not_accepted(monkeypatch, raw):
    calls = fake_http(monkeypatch, raw=raw)
    assert ark.validate_once(KEY, ark.DEFAULT_MODEL)
    assert len(calls) == 1


def test_timeout_and_no_redirect(monkeypatch):
    calls = []
    def timeout(self, request, **kwargs):
        calls.append(request)
        raise urllib.error.URLError(KEY)
    monkeypatch.setattr(ark.urllib.request.OpenerDirector, 'open', timeout)
    reason = ark.validate_once(KEY, ark.DEFAULT_MODEL)
    assert '超时' in reason and KEY not in reason and len(calls) == 1
    assert ark.NoRedirect().redirect_request(None, None, 302, '', {}, 'https://elsewhere') is None


def test_atomic_permissions_and_failed_replace_preserves_previous(tmp_path, monkeypatch):
    path = tmp_path/'ark.env'
    ownership = []
    monkeypatch.setattr(ark.os, 'fchown', lambda *args: ownership.append(args[1:]))
    ark.atomic_write(path, KEY, ark.DEFAULT_MODEL, 987)
    assert path.read_text() == f'ARK_API_KEY={KEY}\nARK_MODEL={ark.DEFAULT_MODEL}\n'
    assert stat.S_IMODE(path.stat().st_mode) == 0o640 and ownership == [(0, 987)]
    before = path.read_bytes()
    def fail(*args):
        raise OSError(KEY)
    monkeypatch.setattr(ark.os, 'replace', fail)
    with pytest.raises(OSError):
        ark.atomic_write(path, KEY+'-new', 'ep-test', 987)
    assert path.read_bytes() == before and list(tmp_path.iterdir()) == [path]
    for key, model in [(KEY+'\nX=bad', ark.DEFAULT_MODEL), (KEY, '$(bad)'), ('', 'ep-test')]:
        with pytest.raises(ValueError):
            ark.atomic_write(path, key, model, 987)
        assert path.read_bytes() == before


def interactive(monkeypatch, tmp_path, answers, existing=False):
    monkeypatch.setattr(ark, 'ARK_FILE', tmp_path/'ark.env')
    monkeypatch.setattr(ark.os, 'geteuid', lambda: 0)
    real_lstat = ark.Path.lstat
    def lstat(path, *args, **kwargs):
        st = real_lstat(path, *args, **kwargs)
        return SimpleNamespace(st_mode=st.st_mode, st_uid=0)
    monkeypatch.setattr(ark.Path, 'lstat', lstat)
    monkeypatch.setattr(ark.grp, 'getgrnam', lambda name: SimpleNamespace(gr_gid=os.getgid()))
    monkeypatch.setattr(ark.resource, 'setrlimit', lambda *args: None)
    monkeypatch.setattr(ark.sys.stdin, 'isatty', lambda: True)
    monkeypatch.setattr(ark.sys.stdout, 'isatty', lambda: True)
    monkeypatch.setattr(ark.os, 'fchown', lambda *args: None)
    iterator = iter(answers)
    monkeypatch.setattr('builtins.input', lambda prompt: next(iterator))
    if existing:
        ark.ARK_FILE.write_text('previous-synthetic-value')


def test_declined_overwrite_never_reads_existing_or_calls_cloud(tmp_path, monkeypatch, capsys):
    interactive(monkeypatch, tmp_path, [''], existing=True)
    monkeypatch.setattr(ark.getpass, 'getpass', lambda *args: pytest.fail('must not prompt for key'))
    monkeypatch.setattr(ark.Path, 'read_text', lambda *args: pytest.fail('must not read existing key'))
    ark.configure()
    assert '已取消' in capsys.readouterr().out


@pytest.mark.parametrize('override', ['', 'ep-user-selected'])
def test_interactive_default_override_and_single_check(tmp_path, monkeypatch, capsys, override):
    interactive(monkeypatch, tmp_path, ['y', override], existing=True)
    monkeypatch.setattr(ark.getpass, 'getpass', lambda *args: KEY)
    calls = fake_http(monkeypatch)
    ark.configure()
    assert capsys.readouterr().out == 'Key 与模型可用\n'
    assert json.loads(calls[0][0].data)['model'] == (override or ark.DEFAULT_MODEL)
    assert len(calls) == 1


def test_exception_never_prints_secret(tmp_path, monkeypatch, capsys):
    interactive(monkeypatch, tmp_path, [])
    def failure(*args):
        raise RuntimeError(KEY)
    monkeypatch.setattr(ark.getpass, 'getpass', failure)
    with pytest.raises(SystemExit):
        ark.configure()
    assert KEY not in capsys.readouterr().out and not ark.ARK_FILE.exists()


def test_non_tty_fails_before_key_prompt(monkeypatch, capsys):
    monkeypatch.setattr(ark.resource, 'setrlimit', lambda *args: None)
    monkeypatch.setattr(ark.sys.stdin, 'isatty', lambda: False)
    monkeypatch.setattr(ark.getpass, 'getpass', lambda *args: pytest.fail('must not prompt'))
    with pytest.raises(SystemExit):
        ark.configure()
    assert '交互式' in capsys.readouterr().out


def test_getpass_echo_fallback_fails_closed(tmp_path, monkeypatch, capsys):
    interactive(monkeypatch, tmp_path, [])
    def fallback(*args):
        warnings.warn('could echo', ark.getpass.GetPassWarning)
        pytest.fail('must stop before fallback reads a key')
    monkeypatch.setattr(ark.getpass, 'getpass', fallback)
    with pytest.raises(SystemExit):
        ark.configure()
    assert not ark.ARK_FILE.exists() and '异常' in capsys.readouterr().out


def test_symlink_refused_before_prompt(tmp_path, monkeypatch, capsys):
    interactive(monkeypatch, tmp_path, [])
    ark.ARK_FILE.symlink_to(tmp_path/'other')
    monkeypatch.setattr(ark.getpass, 'getpass', lambda *args: pytest.fail('must not prompt'))
    with pytest.raises(SystemExit):
        ark.configure()
    assert not (tmp_path/'other').exists() and '异常' in capsys.readouterr().out


def test_saved_configuration_retained_on_http_failure(tmp_path, monkeypatch, capsys):
    interactive(monkeypatch, tmp_path, [''])
    monkeypatch.setattr(ark.getpass, 'getpass', lambda *args: KEY)
    calls = fake_http(monkeypatch, status=404, code='ModelNotOpen')
    with pytest.raises(SystemExit) as exit:
        ark.configure()
    assert exit.value.code == 1 and ark.ARK_FILE.exists() and len(calls) == 1
    output = capsys.readouterr().out
    assert '模型未开通' in output and '配置已保存' in output and KEY not in output
