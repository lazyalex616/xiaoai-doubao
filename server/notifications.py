"""One private, bounded family digest; no transcript or credential in logs."""
import datetime as dt
import json
import logging
import os
import re
import stat
import urllib.error
import urllib.request
from pathlib import Path
from zoneinfo import ZoneInfo

LOG = logging.getLogger(__name__)
SHANGHAI = ZoneInfo('Asia/Shanghai')
PREFIX = 'https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key='


WORD = re.compile(r"[A-Za-z]+(?:[-'][A-Za-z]+)?", re.ASCII)


def plain(value, limit=32):
    text = re.sub(r'https?://\S+|<[^>]*>', '', str(value))
    text = re.sub(r'[`*#\[\]<>@\\\r\n]', ' ', text)
    return text.strip()[:limit] or '孩子'


def valid_word(value):
    return isinstance(value, str) and len(value) <= 20 and WORD.fullmatch(value) is not None


def digest(store, household, date):
    data = store.records(household, date)
    if not data['records']:
        return None
    start = dt.datetime.combine(date, dt.time(), SHANGHAI).timestamp()
    children = store.snapshot(household)['children']
    blocks = [f'## {date.isoformat()} 英语练习小结']
    for child in children:
        records = [r for r in data['records'] if r['child_id'] == child['id']]
        seconds = sum(max(0, min(r['ended_at'], start+86400)-max(r['started_at'], start)) for r in records)
        minutes = '不到 1 分钟' if seconds < 60 else f'{int((seconds+30)//60)} 分钟'
        lines = [f"### {plain(child['profile']['name'], 32)} · {minutes}"]
        if not records:
            lines.append('今天没有记录')
        else:
            words = list(dict.fromkeys(w for r in records if r['summary']
                                       for w in r['summary'].get('new_words', []) if valid_word(w)))[:12]
            if words:
                lines.append('聊过的词：'+'、'.join(words))
        blocks.extend(lines)
    from .config import parent_url
    url = parent_url()
    footer = '\n详情：'+url if url else ''
    text = '\n'.join(blocks)
    # Reserve the final link within WeCom's 4096-byte markdown limit.
    text = text.encode()[:4096-len(footer.encode())].decode('utf-8', 'ignore').rstrip()
    return text+footer


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class WecomSender:
    def __init__(self, url, opener=None):
        self.url = url
        self.opener = opener or urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    @classmethod
    def from_environment(cls):
        filename = os.environ.get('WECOM_WEBHOOK_FILE')
        if not filename:
            return None
        try:
            path = Path(filename)
            mode = path.stat()
            url = path.read_text().strip()
            if (not stat.S_ISREG(mode.st_mode) or stat.S_IMODE(mode.st_mode) != 0o600
                    or not re.fullmatch(re.escape(PREFIX)+r'[A-Za-z0-9_-]+', url)
                    or not os.environ.get('WECOM_HOUSEHOLD')):
                raise ValueError()
        except Exception:
            raise RuntimeError('invalid private WeCom configuration') from None
        return cls(url)

    def send(self, content):
        body = json.dumps({'msgtype': 'markdown', 'markdown': {'content': content}}, ensure_ascii=False).encode()
        request = urllib.request.Request(self.url, data=body, headers={'Content-Type': 'application/json'})
        try:
            with self.opener.open(request, timeout=5) as response:
                result = json.loads(response.read(8192))
            code = result.get('errcode')
            if type(code) is int and code == 0:
                return 'sent'
            return 'retry' if type(code) is int and code in (-1, 45009) else 'failed'
        except urllib.error.HTTPError as exc:
            return 'retry' if exc.code == 429 else 'uncertain'
        except Exception:
            # Timeout/transport errors may follow delivery. Never risk a duplicate.
            return 'uncertain'


class DailyNotifier:
    def __init__(self, store, sender=None, household='', default_time='20:00'):
        if not re.fullmatch(r'(?:[01][0-9]|2[0-3]):[0-5][0-9]', default_time):
            raise ValueError('invalid daily notification time')
        self.store, self.sender, self.household = store, sender, household
        self.default_time = default_time

    def settings(self, household):
        with self.store.db() as db:
            row = db.execute('SELECT enabled,time FROM notification_settings WHERE household_id=?', (household,)).fetchone()
        ready = bool(self.sender and household == self.household)
        return dict(enabled=bool(row['enabled']) if row else ready,
                    time=row['time'] if row else self.default_time, ready=ready, timezone='Asia/Shanghai')

    def save_settings(self, household, enabled, time):
        with self.store.db() as db:
            db.execute('INSERT OR REPLACE INTO notification_settings VALUES(?,?,?)', (household, int(enabled), time))

    def once(self):
        if not self.sender or not self.household:
            return False
        now = int(self.store.now())
        local = dt.datetime.fromtimestamp(now, SHANGHAI)
        settings = self.settings(self.household)
        if not settings['enabled'] or local.strftime('%H:%M') < settings['time']:
            return False
        date = local.date().isoformat()
        # Same lock as deletion: do not build a digest, then send stale deleted
        # content. A sending claim is durable before HTTP; restart cannot resend it.
        with self.store.lock:
            with self.store.db() as db:
                row = db.execute('SELECT * FROM notification_days WHERE household_id=? AND date=?', (self.household, date)).fetchone()
                if row and (row['state'] != 'retry' or now < row['next_at']):
                    return False
                content = digest(self.store, self.household, local.date())
                attempts = (row['attempts'] if row else 0)+1
                state = 'sending' if content else 'skipped'
                db.execute('INSERT OR REPLACE INTO notification_days VALUES(?,?,?,?,?)', (self.household, date, state, attempts, 0))
            if content is None:
                return False
            result = self.sender.send(content)
            state = 'failed' if result == 'retry' and attempts >= 3 else result
            next_at = now + (60 if attempts == 1 else 300) if state == 'retry' else 0
            with self.store.db() as db:
                db.execute('UPDATE notification_days SET state=?,next_at=? WHERE household_id=? AND date=?', (state, next_at, self.household, date))
            # Only fixed state and attempt count: no URL, names or message content.
            LOG.warning('wecom_daily state=%s attempt=%d', state, attempts)
            return state == 'sent'
