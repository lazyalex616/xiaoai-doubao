"""SQLite WAL migration, encrypted payloads, household isolation and deletion epochs."""
import base64
import hashlib
import json
import os
import re
import secrets
import sqlite3
import threading
import time
import datetime
from zoneinfo import ZoneInfo
from contextlib import contextmanager
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

DAY = 86400
PARENT_SESSION_SECONDS = 30 * DAY


def redact(text):
    text = re.sub(r"[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}", "[private]", text)
    text = re.sub(r"(?<!\d)(?:\+?\d[\d ()-]{7,}\d)(?!\d)", "[private]", text)
    text = re.sub(r"(?:我家|住在|地址|学校|电话|密码|API.?Key)\s*[:：是]?[^，。!?\n]*", "[private]", text, flags=re.I)
    text = re.sub(r"(?:my address|my school|my password|my phone|I live at)\s*[^.!?\n]*", "[private]", text, flags=re.I)
    return text


def digest(secret):
    return hashlib.sha256(secret.encode()).hexdigest()


def password_hash(password, salt):
    return hashlib.scrypt(password.encode(), salt=bytes.fromhex(salt), n=16384, r=8, p=1).hex()


class Store:
    def __init__(self, path, key, now=time.time):
        self.path = str(path)
        self.aes = AESGCM(key)
        self.now = now
        self.lock = threading.RLock()
        Path(path).parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with self.db() as db:
            db.executescript('''
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS households(id TEXT PRIMARY KEY, salt TEXT NOT NULL,
              password_hash TEXT NOT NULL, generation INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY, household_id TEXT NOT NULL,
              secret_hash TEXT UNIQUE NOT NULL, revoked INTEGER NOT NULL DEFAULT 0,
              ack_generation INTEGER NOT NULL DEFAULT -1, last_seen INTEGER NOT NULL DEFAULT 0,
              status TEXT NOT NULL DEFAULT 'unknown');
            CREATE TABLE IF NOT EXISTS profiles(id TEXT PRIMARY KEY, household_id TEXT NOT NULL,
              payload BLOB NOT NULL, version INTEGER NOT NULL DEFAULT 1);
            CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY, household_id TEXT NOT NULL,
              device_id TEXT NOT NULL, child_id TEXT NOT NULL, generation INTEGER NOT NULL,
              started_at INTEGER NOT NULL, ended_at INTEGER NOT NULL, payload BLOB,
              payload_hash TEXT NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE IF NOT EXISTS summaries(session_id TEXT PRIMARY KEY, household_id TEXT NOT NULL,
              child_id TEXT NOT NULL, generation INTEGER NOT NULL, created_at INTEGER NOT NULL,
              payload BLOB NOT NULL, status TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS jobs(session_id TEXT PRIMARY KEY, generation INTEGER NOT NULL,
              attempts INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL DEFAULT 'pending');
            CREATE TABLE IF NOT EXISTS parent_tokens(token_hash TEXT PRIMARY KEY, household_id TEXT NOT NULL,
              csrf TEXT NOT NULL, expires_at INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS notification_settings(household_id TEXT PRIMARY KEY,
              enabled INTEGER NOT NULL, time TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS notification_days(household_id TEXT NOT NULL, date TEXT NOT NULL,
              state TEXT NOT NULL, attempts INTEGER NOT NULL, next_at INTEGER NOT NULL,
              PRIMARY KEY(household_id,date));
            PRAGMA user_version=1;
            ''')
        os.chmod(self.path, 0o600)
        self.apply_ledger(self.path+".deletions")
        from .learning import migrate_words
        migrate_words(self)

    @contextmanager
    def db(self):
        with self.lock:
            db = sqlite3.connect(self.path, timeout=5)
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA foreign_keys=ON")
            try:
                with db:
                    yield db
            finally:
                db.close()

    def seal(self, value):
        nonce = secrets.token_bytes(12)
        return nonce + self.aes.encrypt(nonce, json.dumps(value, ensure_ascii=False).encode(), b"xiaoai-memory-v1")

    def unseal(self, value):
        return json.loads(self.aes.decrypt(value[:12], value[12:], b"xiaoai-memory-v1"))

    def provision(self, household, password, device, names=()):
        secret = secrets.token_hex(32)
        salt = secrets.token_hex(16)
        with self.db() as db:
            db.execute("INSERT INTO households(id,salt,password_hash) VALUES(?,?,?)",
                       (household, salt, password_hash(password, salt) if password is not None else '!disabled'))
            db.execute("INSERT INTO devices(id,household_id,secret_hash) VALUES(?,?,?)",
                       (device, household, digest(secret)))
            for name in names:
                profile = dict(name=name, aliases=[name], grade="", level="", interests="", goals="")
                db.execute("INSERT INTO profiles(id,household_id,payload) VALUES(?,?,?)",
                           (secrets.token_hex(16), household, self.seal(profile)))
        return secret

    def set_parent_password(self, household, password):
        if not 8 <= len(password) <= 256:
            raise ValueError('password must contain 8 to 256 characters')
        salt = secrets.token_hex(16)
        with self.db() as db:
            if not db.execute('UPDATE households SET salt=?,password_hash=? WHERE id=?',
                              (salt, password_hash(password, salt), household)).rowcount:
                raise ValueError('unknown household')
            db.execute('DELETE FROM parent_tokens WHERE household_id=?', (household,))

    def device(self, secret):
        with self.db() as db:
            row = db.execute("SELECT * FROM devices WHERE secret_hash=? AND revoked=0", (digest(secret),)).fetchone()
            return dict(row) if row else None

    def login(self, household, password):
        with self.db() as db:
            row = db.execute("SELECT * FROM households WHERE id=?", (household,)).fetchone()
            # Constant work even for unknown household.
            computed = password_hash(password, row['salt'] if row else '00'*16)
            if not row or not secrets.compare_digest(row['password_hash'], computed):
                return None
            token, csrf = secrets.token_hex(32), secrets.token_hex(32)
            db.execute("DELETE FROM parent_tokens WHERE expires_at<?", (int(self.now()),))
            db.execute("INSERT INTO parent_tokens VALUES(?,?,?,?)",
                       (digest(token), household, csrf, int(self.now()) + PARENT_SESSION_SECONDS))
            return token, csrf

    def parent(self, token):
        with self.db() as db:
            row = db.execute("SELECT * FROM parent_tokens WHERE token_hash=? AND expires_at>?",
                             (digest(token), int(self.now()))).fetchone()
            return dict(row) if row else None

    def snapshot(self, household):
        now = int(self.now())
        with self.db() as db:
            gen = db.execute("SELECT generation FROM households WHERE id=?", (household,)).fetchone()[0]
            children = []
            for row in db.execute("SELECT * FROM profiles WHERE household_id=? ORDER BY rowid", (household,)):
                # One continuation card, derived from its source summary: no extra
                # table, deletion state or retention clock to get out of sync.
                summaries = []
                for s in db.execute("SELECT m.*,s.started_at FROM summaries m JOIN sessions s ON s.id=m.session_id WHERE m.household_id=? AND m.child_id=? AND m.created_at>? AND s.deleted=0 ORDER BY s.started_at DESC,s.id DESC LIMIT 1",
                                    (household, row['id'], now-90*DAY)):
                    saved = self.unseal(s['payload'])
                    summary = {k: saved[k] for k in ('topic', 'recap', 'resume_question', 'new_words')}
                    summary['new_words'] = summary['new_words'][:3]
                    date = datetime.datetime.fromtimestamp(s['started_at'], ZoneInfo('Asia/Shanghai')).date().isoformat()
                    summaries.append(dict(summary, status=s['status'], session_id=s['session_id'], date=date))
                child = dict(id=row['id'], profile=self.unseal(row['payload']), version=row['version'], summaries=summaries)
                latest = db.execute("SELECT id,payload FROM sessions WHERE household_id=? AND child_id=? AND deleted=0 AND payload IS NOT NULL ORDER BY started_at DESC,id DESC LIMIT 1",(household,row['id'])).fetchone()
                if latest and not db.execute('SELECT 1 FROM summaries WHERE session_id=?',(latest['id'],)).fetchone():
                    turns = self.unseal(latest['payload'])['turns']
                    pairs = []
                    for a in turns:
                        if a['role']=='assistant' and a.get('question_id') and not any(a.get(f) for f in ('interrupted','incomplete','truncated')):
                            for u in turns:
                                if u['role']=='user' and u.get('item_id')==a['question_id'] and not any(u.get(f) for f in ('incomplete','truncated')):
                                    pairs.append(u['text']+' / '+a['text'])
                    if pairs: child['temporary'] = 'Temporary continuation, summary pending: '+ ' | '.join(pairs[-2:])[:250]
                children.append(child)
            return dict(generation=gen, expires_at=now+DAY, children=children)

    def upload(self, device, upload):
        data = upload.model_dump()
        # Deletion and uploads serialize in one transaction.
        now = int(self.now())
        if data['ended_at'] < data['started_at'] or data['ended_at'] > now+60 or data['started_at'] < now-30*DAY:
            return 410, {'error': 'expired_or_invalid_time'}
        if len({t['seq'] for t in data['turns']}) != len(data['turns']):
            return 400, {'error': 'duplicate_sequence'}
        for t in data['turns']:
            t['text'] = redact(t['text'])
        canonical = json.dumps(data, sort_keys=True, ensure_ascii=False).encode()
        if len(canonical) > 256*1024:
            return 413, {'error': 'session_too_large'}
        checksum = hashlib.sha256(canonical).hexdigest()
        h = device['household_id']
        with self.db() as db:
            gen = db.execute("SELECT generation FROM households WHERE id=?", (h,)).fetchone()[0]
            if data['generation'] != gen:
                return 409, {'error': 'generation_changed', 'generation': gen}
            if not db.execute("SELECT 1 FROM profiles WHERE id=? AND household_id=?", (data['child_id'], h)).fetchone():
                return 403, {'error': 'child_not_bound'}
            old = db.execute("SELECT * FROM sessions WHERE id=?", (data['session_id'],)).fetchone()
            if old:
                if old['household_id'] != h or old['device_id'] != device['id']:
                    return 403, {'error': 'session_not_bound'}
                if old['deleted'] or old['payload'] is None:
                    return 410, {'error': 'deleted_or_expired'}
                if old['payload_hash'] != checksum:
                    return 409, {'error': 'idempotency_conflict'}
                return 200, {'ack': data['session_id']}
            db.execute("INSERT INTO sessions(id,household_id,device_id,child_id,generation,started_at,ended_at,payload,payload_hash) VALUES(?,?,?,?,?,?,?,?,?)",
                       (data['session_id'], h, device['id'], data['child_id'], gen, data['started_at'], data['ended_at'], self.seal(data), checksum))
            db.execute("INSERT INTO jobs(session_id,generation) VALUES(?,?)", (data['session_id'], gen))
            return 200, {'ack': data['session_id']}

    def records(self, h, date=None):
        date = date or datetime.datetime.fromtimestamp(self.now(), ZoneInfo('Asia/Shanghai')).date()
        start = int(datetime.datetime.combine(date, datetime.time(), ZoneInfo('Asia/Shanghai')).timestamp())
        end = int(datetime.datetime.combine(date+datetime.timedelta(days=1), datetime.time(), ZoneInfo('Asia/Shanghai')).timestamp())
        with self.db() as db:
            result = []
            for row in db.execute("SELECT * FROM sessions WHERE household_id=? AND deleted=0 AND started_at<? AND (ended_at>? OR started_at>=?) ORDER BY started_at DESC,id DESC", (h,end,start,start)):
                s = db.execute("SELECT * FROM summaries WHERE session_id=?", (row['id'],)).fetchone()
                waiting = row['payload'] and db.execute("SELECT 1 FROM jobs WHERE session_id=? AND state IN ('pending','running')", (row['id'],)).fetchone()
                result.append(dict(session_id=row['id'], child_id=row['child_id'], started_at=row['started_at'],
                                   ended_at=row['ended_at'],
                                   transcript=self.unseal(row['payload']) if row['payload'] else None,
                                   summary=self.unseal(s['payload']) if s else None, status=s['status'] if s else 'pending' if waiting else 'expired'))
            devices = [dict(row) for row in db.execute("SELECT id,ack_generation,last_seen,status FROM devices WHERE household_id=?", (h,))]
            return dict(records=result, devices=devices, date=date.isoformat())

    def practice(self, household, days):
        timezone = ZoneInfo('Asia/Shanghai')
        today = datetime.datetime.fromtimestamp(self.now(), timezone).date()
        first = today-datetime.timedelta(days=days-1)
        start = int(datetime.datetime.combine(first, datetime.time(), timezone).timestamp())
        end = start+days*DAY
        with self.db() as db:
            children = [row['id'] for row in db.execute('SELECT id FROM profiles WHERE household_id=?', (household,))]
            totals = [dict.fromkeys(children, 0) for _ in range(days)]
            for row in db.execute('SELECT child_id,started_at,ended_at FROM sessions WHERE household_id=? AND deleted=0 AND started_at<? AND ended_at>?', (household,end,start)):
                if row['child_id'] not in totals[0]:
                    continue
                for i, daily in enumerate(totals):
                    day_start = start+i*DAY
                    daily[row['child_id']] += max(0, min(row['ended_at'], day_start+DAY)-max(row['started_at'], day_start))
        return dict(date=today.isoformat(), timezone='Asia/Shanghai', days=[
            dict(date=(first+datetime.timedelta(days=i)).isoformat(), minutes={child: seconds/60 for child,seconds in daily.items()})
            for i,daily in enumerate(totals)])

    def append_deletion(self, entry):
        # Write-ahead intent: durable before SQLite deletion. Latest ledger must be
        # copied separately from rolling backups; it carries no conversation text.
        fd = os.open(self.path+'.deletions', os.O_WRONLY|os.O_CREAT|os.O_APPEND|os.O_NOFOLLOW, 0o600)
        try:
            content = base64.b64encode(self.seal(entry))+b'\n'
            pos = 0
            while pos < len(content):
                pos += os.write(fd,content[pos:])
            os.fsync(fd)
        finally:
            os.close(fd)

    def apply_ledger(self, path):
        if not Path(path).exists():
            return
        with self.db() as db:
            for line in Path(path).read_bytes().splitlines():
                entry = self.unseal(base64.b64decode(line,validate=True))
                h, gen = entry['household'], entry['generation']
                for sid in entry['sessions']:
                    db.execute('UPDATE sessions SET payload=NULL,deleted=1 WHERE id=? AND household_id=?',(sid,h))
                    db.execute('DELETE FROM summaries WHERE session_id=? AND household_id=?',(sid,h))
                    db.execute('DELETE FROM jobs WHERE session_id IN (SELECT id FROM sessions WHERE id=? AND household_id=?)',(sid,h))
                db.execute('UPDATE households SET generation=max(generation,?) WHERE id=?',(gen,h))
            # Rebase only nondeleted server records, never client pending uploads.
            db.execute('UPDATE sessions SET generation=(SELECT generation FROM households WHERE id=sessions.household_id) WHERE deleted=0')
            db.execute('UPDATE jobs SET generation=(SELECT generation FROM sessions WHERE id=jobs.session_id)')

    def delete(self, h, child_id=None, session_id=None):
        with self.db() as db:
            if child_id and not db.execute("SELECT 1 FROM profiles WHERE id=? AND household_id=?", (child_id,h)).fetchone():
                return False
            if session_id and not db.execute("SELECT 1 FROM sessions WHERE id=? AND household_id=?", (session_id,h)).fetchone():
                return False
            clause, args = ('child_id=?', [child_id]) if child_id else ('id=?', [session_id])
            ids = [r[0] for r in db.execute(f"SELECT id FROM sessions WHERE household_id=? AND {clause}", [h]+args)]
            gen = db.execute('SELECT generation FROM households WHERE id=?',(h,)).fetchone()[0]+1
            self.append_deletion(dict(household=h,generation=gen,sessions=ids))
            for sid in ids:
                db.execute("UPDATE sessions SET payload=NULL,deleted=1 WHERE id=?", (sid,))
                db.execute("DELETE FROM summaries WHERE session_id=?", (sid,))
                db.execute("DELETE FROM jobs WHERE session_id=?", (sid,))
            db.execute("UPDATE households SET generation=? WHERE id=?", (gen,h))
            db.execute('UPDATE sessions SET generation=? WHERE household_id=? AND deleted=0',(gen,h))
            db.execute('UPDATE jobs SET generation=? WHERE session_id IN (SELECT id FROM sessions WHERE household_id=? AND deleted=0)',(gen,h))
            return True

    def edit_profile(self, h, child_id, profile):
        data = profile.model_dump()
        data = {k: [redact(s) for s in v] if isinstance(v,list) else redact(v) for k,v in data.items()}
        with self.db() as db:
            return db.execute("UPDATE profiles SET payload=?,version=version+1 WHERE id=? AND household_id=?",
                              (self.seal(data),child_id,h)).rowcount == 1

    def cleanup(self):
        now = int(self.now())
        with self.db() as db:
            db.execute("DELETE FROM jobs WHERE session_id IN (SELECT id FROM sessions WHERE started_at<?)", (now-30*DAY,))
            db.execute("UPDATE sessions SET payload=NULL WHERE started_at<?", (now-30*DAY,))
            db.execute("DELETE FROM summaries WHERE created_at<?", (now-90*DAY,))
            db.execute("DELETE FROM parent_tokens WHERE expires_at<?", (now,))
            # Tombstones contain no text and are intentionally not expired.
