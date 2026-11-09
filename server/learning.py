"""Child learning view, using Shanghai calendar days and existing summaries."""
import datetime as dt
import re
from zoneinfo import ZoneInfo

TZ = ZoneInfo('Asia/Shanghai')
DAY = dt.timedelta(days=1)


def local_date(timestamp):
    return dt.datetime.fromtimestamp(timestamp, TZ).date()


def words(summary):
    return sorted({w.strip().lower() for w in summary.get('new_words', [])
                   if re.fullmatch(r"[A-Za-z]+(?:[-'][A-Za-z]+)*", w.strip())})


def remember_words(db, child_id, summary, started_at):
    date = local_date(started_at).isoformat()
    db.executemany('''INSERT INTO first_words VALUES(?,?,?) ON CONFLICT(child_id,word)
                      DO UPDATE SET first_date=min(first_date,excluded.first_date)''',
                   [(child_id, word, date) for word in words(summary)])


def migrate_words(store):
    with store.db() as db:
        if db.execute("SELECT 1 FROM sqlite_master WHERE name='first_words'").fetchone():
            return
        db.execute('BEGIN IMMEDIATE')
        db.execute('''CREATE TABLE first_words(child_id TEXT NOT NULL, word TEXT NOT NULL,
                      first_date TEXT NOT NULL, PRIMARY KEY(child_id,word))''')
        for row in db.execute('''SELECT m.child_id,m.payload,s.started_at FROM summaries m
                                 JOIN sessions s ON s.id=m.session_id WHERE s.deleted=0'''):
            remember_words(db, row['child_id'], store.unseal(row['payload']), row['started_at'])


def children(store, household):
    with store.db() as db:
        return [dict(id=r['id'], profile=store.unseal(r['payload']), latest_at=r['latest_at'])
                for r in db.execute('''SELECT p.*, (SELECT max(s.started_at) FROM sessions s
                  WHERE s.child_id=p.id AND s.household_id=p.household_id AND s.deleted=0) latest_at
                  FROM profiles p WHERE p.household_id=? ORDER BY p.id''', (household,))]


def session(store, row):
    transcript = store.unseal(row['payload']) if row['payload'] and row['started_at'] >= store.now()-30*86400 else None
    summary = store.unseal(row['summary_payload']) if row['summary_payload'] and row['summary_created_at'] >= store.now()-90*86400 else None
    # Only exact, complete user quotes qualify; never fall back to model prose.
    quotes = re.findall(r'[“"]([^”"]+)[”"]', (summary or {}).get('parent_highlight', ''))
    users = [t['text'] for t in (transcript or {}).get('turns', []) if t['role'] == 'user'
             and not any(t.get(flag) for flag in ('incomplete', 'truncated', 'interrupted'))]
    english = [q for q in quotes if re.search('[A-Za-z]', q) and not re.search(r'[\u3400-\u9fff]', q)
               and any(q in text for text in users)] if row['status'] != 'fallback' else []
    return dict(session_id=row['id'], child_id=row['child_id'], started_at=row['started_at'],
                ended_at=row['ended_at'], summary=summary, transcript=transcript,
                status=row['status'] or ('pending' if row['waiting'] and transcript else 'expired'),
                child_english=list(dict.fromkeys(english)))


def sessions(store, household, child_id):
    with store.db() as db:
        return [session(store, r) for r in db.execute('''SELECT s.*,m.payload summary_payload,m.created_at summary_created_at,m.status,
                  EXISTS(SELECT 1 FROM jobs j WHERE j.session_id=s.id AND j.state IN ('pending','running')) waiting
                  FROM sessions s LEFT JOIN summaries m ON m.session_id=s.id
                  WHERE s.household_id=? AND s.child_id=? AND s.deleted=0 ORDER BY s.started_at DESC,s.id DESC''',
                  (household, child_id))]


def daily(records, first, last):
    result = {}
    for record in records:
        start, end = record['started_at'], record['ended_at']
        date = max(first, local_date(start))
        final = min(last, local_date(max(start, end-1)))
        while date <= final:
            midnight = int(dt.datetime.combine(date, dt.time(), TZ).timestamp())
            seconds = max(0, min(end, midnight+86400)-max(start, midnight))
            day = result.setdefault(date.isoformat(), dict(date=date.isoformat(), seconds=0, records=[]))
            day['seconds'] += seconds
            day['records'].append(dict(record, seconds=seconds))
            date += DAY
    return result


def overview(store, household, child_id):
    records = sessions(store, household, child_id)
    today = local_date(store.now())
    monday = today-dt.timedelta(days=today.weekday())
    days = daily(records, today-dt.timedelta(days=60), today)
    week = [dict(date=(monday+dt.timedelta(days=i)).isoformat(), seconds=days.get(
        (monday+dt.timedelta(days=i)).isoformat(), {}).get('seconds', 0)) for i in range(7)]
    previous = [days.get((monday-dt.timedelta(days=7)+dt.timedelta(days=i)).isoformat())
                for i in range(today.weekday()+1)]
    date = today if days.get(today.isoformat(), {}).get('seconds', 0) else today-DAY
    streak = 0
    while days.get(date.isoformat(), {}).get('seconds', 0) and streak < 60:
        streak += 1
        date -= DAY
    weekly_words = sorted({word for record in records if monday <= local_date(record['started_at']) <= today
                           for word in words(record['summary'] or {})})
    with store.db() as db:
        first_dates = {r['word']: r['first_date'] for r in db.execute(
            'SELECT word,first_date FROM first_words WHERE child_id=?', (child_id,))}
    expression = next((r for r in records if r['child_english']), None)
    if expression is None:
        expression = next((r for r in records if r['status'] != 'fallback' and r['summary']
                           and r['summary'].get('parent_help') not in (None, '', '未观察到')), None)
    return dict(date=today.isoformat(), latest=records[0] if records else None,
                expression=expression,
                today_count=len(days.get(today.isoformat(), {}).get('records', [])),
                today_seconds=days.get(today.isoformat(), {}).get('seconds', 0), week=week,
                week_days=sum(day['seconds'] > 0 for day in week),
                week_seconds=sum(day['seconds'] for day in week),
                previous_seconds=sum(day['seconds'] for day in previous if day),
                previous_has_records=any(previous), streak=streak,
                words=[dict(word=w, first=monday.isoformat() <= first_dates.get(w, '') <= today.isoformat())
                       for w in weekly_words])


def history(store, household, child_id, before=None):
    records = sessions(store, household, child_id)
    today = local_date(store.now())
    first = min((local_date(r['started_at']) for r in records), default=today)
    groups = daily(records, first, today)
    dates = sorted((d for d in groups if before is None or d < before.isoformat()), reverse=True)
    selected = dates[:7]
    return dict(days=[groups[d] for d in selected], next_before=selected[-1] if len(dates) > 7 else None)
