"""Single serial worker with bounded company Ark summaries and local fallback."""
import logging
import re
import threading

from .ark_summary import ArkFailure, ArkSummarizer
from .models import Summary
from .store import redact
from .learning import remember_words


class MockSummarizer:
    name = 'mock-v1'

    def summarize(self, turns):
        users = [t['text'] for t in turns if t['role'] == 'user']
        text = ' | '.join(users) if users else 'English practice'
        words = list(dict.fromkeys(re.findall(r'\b[A-Za-z]{4,12}\b', text.lower())))[:3]
        recap = text if len(text) <= 200 else text[:140] + ' … ' + text[-55:]
        return dict(topic='English practice', recap=recap,
                    resume_question='Would you like to continue our English practice?', new_words=words)


class Worker:
    def __init__(self, store, summarizer=None, notifier=None):
        self.store = store
        self.model = summarizer or MockSummarizer()
        self.notifier = notifier
        self.stop_event = threading.Event()
        self.thread = None
        self.serial = threading.Lock()
        with store.db() as db:
            db.execute("UPDATE jobs SET state='pending' WHERE state='running'")

    def once(self):
        with self.serial:
            return self._once()

    def _once(self):
        with self.store.db() as db:
            row = db.execute("SELECT s.*,j.attempts FROM jobs j JOIN sessions s ON s.id=j.session_id WHERE j.state='pending' ORDER BY s.started_at LIMIT 1").fetchone()
            if not row:
                return False
            gen = db.execute("SELECT generation FROM households WHERE id=?", (row['household_id'],)).fetchone()[0]
            if row['deleted'] or not row['payload'] or row['generation'] != gen:
                db.execute("DELETE FROM jobs WHERE session_id=?", (row['id'],))
                return True
            db.execute("UPDATE jobs SET state='running',attempts=attempts+1 WHERE session_id=?", (row['id'],))
            turns = self.store.unseal(row['payload'])['turns']
            # Names and profile data are not passed to the summary model.
            names = []
            for p in db.execute("SELECT payload FROM profiles WHERE household_id=?", (row['household_id'],)):
                profile = self.store.unseal(p[0])
                names += [profile['name']] + profile['aliases']
            for t in turns:
                for name in names:
                    if name:
                        t['text'] = re.sub(re.escape(name), 'child', t['text'], flags=re.I)
        status = self.model.name
        try:
            result = Summary.model_validate(self.model.summarize(turns)).model_dump()
            if any(len(w)>32 for w in result['new_words']):
                raise ValueError('word_too_long')
            result = {k: [redact(w) for w in v] if isinstance(v,list) else redact(v) for k,v in result.items()}
        except Exception as exc:
            if not isinstance(exc, ArkFailure) and row['attempts'] < 1:
                with self.store.db() as db:
                    db.execute("UPDATE jobs SET state='pending' WHERE session_id=?", (row['id'],))
                return True
            reason = str(exc) if isinstance(exc, ArkFailure) else type(exc).__name__
            logging.getLogger(__name__).warning('summary_fallback session=%s reason=%s', row['id'][:8], reason)
            result = MockSummarizer().summarize(turns)
            result['recap'] = 'Summary unavailable; temporary continuation: ' + result['recap']
            status = 'fallback'
        # Recheck deletion after model returns, in same write transaction as insert.
        with self.store.db() as db:
            current = db.execute("SELECT s.deleted,s.payload,h.generation FROM sessions s JOIN households h ON s.household_id=h.id WHERE s.id=?", (row['id'],)).fetchone()
            job = db.execute("SELECT 1 FROM jobs WHERE session_id=?", (row['id'],)).fetchone()
            if current and job and not current['deleted'] and current['payload'] and current['generation'] == row['generation']:
                db.execute("INSERT OR REPLACE INTO summaries VALUES(?,?,?,?,?,?,?)", (row['id'], row['household_id'], row['child_id'], row['generation'], int(self.store.now()), self.store.seal(result), status))
                remember_words(db, row['child_id'], result, row['started_at'])
            if current and job and not current['deleted'] and current['payload'] and current['generation'] != row['generation']:
                db.execute("UPDATE jobs SET state='pending',generation=? WHERE session_id=?",(current['generation'],row['id']))
            else:
                db.execute("DELETE FROM jobs WHERE session_id=?", (row['id'],))
        return True

    def start(self):
        def loop():
            self.store.cleanup()
            ticks = 0
            while not self.stop_event.wait(1):
                try:
                    self.once()
                    if self.notifier and ticks % 30 == 0:
                        self.notifier.once()
                    ticks += 1
                    if ticks >= 3600:
                        self.store.cleanup()
                        ticks = 0
                except Exception:
                    # No provider messages or children's text in logs.
                    continue
        self.thread = threading.Thread(target=loop, name='summary-worker', daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=3)
