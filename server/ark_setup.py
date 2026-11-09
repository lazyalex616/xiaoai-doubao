"""Interactive root-only Ark setup. Never render secrets or raw HTTP errors."""
import fcntl
import getpass
import json
import os
from pathlib import Path
import re
import resource
import stat
import sys
import tempfile
import urllib.error
import urllib.request
import warnings
import grp

# Official Ark model/deep-thinking docs, checked 2026-10-02 (see README).
DEFAULT_MODEL = 'doubao-seed-2-0-mini-260428'
ARK_FILE = Path('/etc/xiaoai-memory/ark.env')
ARK_URL = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions'


def atomic_write(path, key, model, gid):
    # Restricted ASCII values are valid for systemd EnvironmentFile and prevent
    # newline/shell injection; the file is never sourced by the admin helper.
    if not re.fullmatch(r'[A-Za-z0-9_.-]{8,512}', key):
        raise ValueError('Key 格式无效，请重新复制完整 Key')
    if not re.fullmatch(r'[A-Za-z0-9_.:/-]{1,160}', model):
        raise ValueError('模型 ID 格式无效')
    fd, name = tempfile.mkstemp(prefix='.ark-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            os.fchown(stream.fileno(), 0, gid)
            os.fchmod(stream.fileno(), 0o640)
            stream.write(f'ARK_API_KEY={key}\nARK_MODEL={model}\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def failure_reason(status, raw):
    try:
        code = json.loads(raw)['error']['code']
    except (ValueError, KeyError, TypeError):
        code = None
    # Only fixed local strings may reach terminal; never the remote message/code.
    if code in ('AccountOverdueError', 'OperationDenied.ServiceOverdue'):
        return '余额不足或账号欠费，请在火山费用中心充值'
    if code == 'ModelNotOpen':
        return '模型未开通，请在方舟控制台开通所选模型'
    if code == 'InvalidEndpointOrModel.ModelIDAccessDisabled':
        return '账号不支持模型 ID 直调，请使用自建接入点 ID'
    if status == 401 or code == 'AuthenticationError':
        return '鉴权失败，请检查 Key 是否正确、有效'
    if status == 404:
        return '模型或接入点不存在，或当前项目无访问权限'
    if status == 403:
        return '无访问权限，请检查 default 项目授权或模型开通状态'
    if status == 429:
        return '请求限流或配额不足'
    if status >= 500:
        return '方舟服务暂时不可用'
    return '请求被拒绝，请检查模型是否支持此接口和参数'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def validate_once(key, model):
    payload = {'model': model, 'messages': [{'role': 'user', 'content': 'Reply only ok.'}],
               'max_tokens': 8, 'stream': False, 'thinking': {'type': 'disabled'}}
    request = urllib.request.Request(ARK_URL, data=json.dumps(payload).encode(),
                                     headers={'Authorization': 'Bearer '+key,
                                              'Content-Type': 'application/json'})
    # No SDK retries, redirects or environment proxies: at most one HTTPS POST.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(request, timeout=15) as response:
            raw = response.read(16385)
            if response.status != 200:
                return failure_reason(response.status, raw)
            if len(raw) > 16384:
                return '校验响应过大'
            content = json.loads(raw)['choices'][0]['message']['content']
            if not isinstance(content, str) or not content.strip():
                return '模型未返回有效文字'
    except urllib.error.HTTPError as exc:
        try:
            return failure_reason(exc.code, exc.read(16384))
        except Exception:
            return '校验响应读取失败；未自动重试'
        finally:
            exc.close()
    except (urllib.error.URLError, TimeoutError, OSError):
        return '网络或 TLS 连接失败/超时；未自动重试'
    except (ValueError, KeyError, IndexError, TypeError):
        return '校验响应格式无效'
    return None


def configure():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if os.geteuid() != 0 or not sys.stdin.isatty() or not sys.stdout.isatty():
        print('失败原因：请使用 sudo 和交互式 SSH 终端运行。')
        raise SystemExit(2)
    try:
        gid = grp.getgrnam('xiaoai').gr_gid
        parent = ARK_FILE.parent.lstat()
        if not stat.S_ISDIR(parent.st_mode) or parent.st_uid != 0 or parent.st_mode & 0o022:
            raise ValueError('配置目录权限不安全')
        # Serialize prompts/replacements without opening an existing Key file.
        fd = os.open(ARK_FILE.parent/'.ark-key.lock',
                     os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if os.path.lexists(ARK_FILE):
                old = ARK_FILE.lstat()
                if not stat.S_ISREG(old.st_mode) or old.st_uid != 0:
                    raise ValueError('已有配置文件类型或属主不安全')
                if input('已有方舟配置，是否覆盖？[y/N]: ').strip().lower() != 'y':
                    print('已取消，配置未改动。')
                    return
            # Turn getpass fallback-to-echo into a hard failure.
            with warnings.catch_warnings():
                warnings.simplefilter('error', getpass.GetPassWarning)
                key = getpass.getpass('粘贴方舟 API Key（不回显）: ').strip()
            model = input(f'模型 ID（回车使用 {DEFAULT_MODEL}）: ').strip() or DEFAULT_MODEL
            atomic_write(ARK_FILE, key, model, gid)
            reason = validate_once(key, model)
            print('Key 与模型可用' if reason is None else '失败原因：'+reason+'（配置已保存）')
            if reason is not None:
                raise SystemExit(1)
    except (KeyboardInterrupt, EOFError):
        print('\n已取消；未自动重试。')
        raise SystemExit(2)
    except Exception:
        # Exceptions can contain credentials; never print repr/traceback.
        print('失败原因：输入格式、文件权限或录入环境异常；未自动重试。')
        raise SystemExit(2)
