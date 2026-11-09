import datetime
import os
import time

import pytest
from fastapi.testclient import TestClient

from server.app import create_app
from server.models import Upload
from server.store import DAY, Store
from server.worker import Worker

ORIGIN='https://xiaoai.test'

@pytest.fixture
def system(tmp_path):
    now=[int(time.time())]
    store=Store(tmp_path/'memory.sqlite',os.urandom(32),now=lambda:now[0])
    a=store.provision('family-a','a-long-parent-password','speaker-a', ['Amy','Ben'])
    b=store.provision('family-b','another-parent-password','speaker-b', ['Amy','Ben'])
    app=create_app(store,origin=ORIGIN,background=False)
    with TestClient(app,base_url=ORIGIN) as client:
        yield store,client,a,b,now,app.state.worker


def headers(secret): return {'Authorization':'Bearer '+secret}

def payload(store, h='family-a', sid='session-a'):
    snap=store.snapshot(h)
    child=next(c for c in snap['children'] if c['profile']['name']=='Amy')
    return dict(session_id=sid,child_id=child['id'],generation=snap['generation'],profile_version=child['version'],
                started_at=int(store.now()),ended_at=int(store.now()),complete=True,
                turns=[dict(seq=0,role='user',text='I like dinosaurs.',event_id='e1',item_id='q1'),
                       dict(seq=1,role='assistant',text='Which dinosaur?',event_id='e2',response_id='r1',question_id='q1')])


def parent(client,h='family-a',password='a-long-parent-password'):
    r=client.post('/parent/login',headers={'Origin':ORIGIN},json={'household':h,'password':password})
    assert r.status_code==200
    return {'Origin':ORIGIN,'X-CSRF-Token':r.json()['csrf']}


def test_device_auth_and_isolation(system):
    store,c,a,b,_,_=system
    assert c.get('/device/snapshot').status_code==401
    assert c.get('/device/snapshot',headers=headers('bad')).status_code==401
    data=c.get('/device/snapshot',headers=headers(a)).json()
    assert len(data['children'])==2
    p=payload(store,'family-b')
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==403
    p=payload(store);p['household_id']='family-b'
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==422
    assert c.get('/parent/data',headers=headers(a)).status_code==401
    with store.db() as db:
        stored=db.execute('SELECT secret_hash FROM devices WHERE id="speaker-a"').fetchone()[0]
        assert stored!=a and len(stored)==64
        db.execute('UPDATE devices SET revoked=1 WHERE id="speaker-a"')
    assert c.get('/device/snapshot',headers=headers(a)).status_code==401


def test_upload_idempotency_and_conflict(system):
    store,c,a,b,_,worker=system
    p=payload(store)
    for _ in range(2): assert c.post('/device/sessions',headers=headers(a),json=p).json()['ack']=='session-a'
    with store.db() as db:
        assert db.execute('SELECT count(*) FROM sessions').fetchone()[0]==1
        assert db.execute('SELECT count(*) FROM jobs').fetchone()[0]==1
    p['turns'][0]['text']='conflicting retry'
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==409
    foreign=payload(store,'family-b')
    assert c.post('/device/sessions',headers=headers(b),json=foreign).status_code==403
    assert worker.once()
    summary=store.snapshot('family-a')['children']
    assert any(ch['summaries'] for ch in summary)


def test_delete_no_resurrection_and_csrf(system):
    store,c,a,_,_,worker=system
    p=payload(store);assert c.post('/device/sessions',headers=headers(a),json=p).status_code==200
    auth=parent(c)
    assert c.delete('/parent/sessions/session-a').status_code==403
    foreign_id=store.snapshot('family-b')['children'][0]['id']
    assert c.delete('/parent/children/'+foreign_id+'/records',headers=auth).status_code==404
    assert c.delete('/parent/sessions/session-a',headers=auth).status_code==200
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==409
    p['generation']=1
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==410
    assert not worker.once()
    with store.db() as db:
        row=db.execute('SELECT payload,deleted FROM sessions').fetchone()
        assert row['payload'] is None and row['deleted']==1
    data=c.get('/parent/data').json()
    assert data['devices'][0]['ack_generation']!=data['generation']
    assert c.post('/device/ack',headers=headers(a),json={'generation':1,'status':'ok'}).status_code==200
    assert c.get('/parent/data').json()['devices'][0]['ack_generation']==1


def test_delete_during_summary(system):
    store,c,a,_,_,_=system
    p=payload(store);store.upload(store.device(a),Upload.model_validate(p))
    class DeleteDuring:
        name='test'
        def summarize(self,turns):
            store.delete('family-a',session_id='session-a')
            return dict(topic='late',recap='must not return',resume_question='',new_words=[])
    w=Worker(store,DeleteDuring());assert w.once()
    assert not any(c['summaries'] for c in store.snapshot('family-a')['children'])


def test_retention_no_replay(system):
    store,c,a,_,now,w=system
    date=datetime.datetime.fromtimestamp(now[0], datetime.timezone(datetime.timedelta(hours=8))).date()
    p=payload(store);store.upload(store.device(a),Upload.model_validate(p))
    assert store.records('family-a',date)['records'][0]['status']=='pending'
    assert w.once()
    store.upload(store.device(a),Upload.model_validate(payload(store,sid='waiting')))
    with store.db() as db:
        db.execute("UPDATE jobs SET state='running' WHERE session_id='waiting'")
    assert next(r for r in store.records('family-a',date)['records'] if r['session_id']=='waiting')['status']=='pending'
    now[0]+=31*DAY;store.cleanup()
    records={r['session_id']:r for r in store.records('family-a',date)['records']}
    assert records['session-a']['transcript'] is None and records['session-a']['summary']
    assert records['waiting']['status']=='expired' and records['waiting']['summary'] is None
    p['started_at']=now[0];p['ended_at']=now[0]
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==410
    now[0]+=60*DAY;store.cleanup()
    assert not any(c['summaries'] for c in store.snapshot('family-a')['children'])
    assert all(r['status']=='expired' and r['summary'] is None for r in store.records('family-a',date)['records'])


def test_summary_validation_and_failure_fallback(system, caplog):
    store,c,a,_,_,_=system
    store.upload(store.device(a),Upload.model_validate(payload(store)))
    class Invalid:
        name='invalid'
        def summarize(self,turns): return {'topic':'bad','extra':'not allowed'}
    w=Worker(store,Invalid());assert w.once()
    assert 'summary_fallback' not in caplog.text
    assert w.once();assert not w.once()
    assert 'summary_fallback session=session- reason=ValidationError' in caplog.text
    assert 'I like dinosaurs.' not in caplog.text and 'not allowed' not in caplog.text
    summary=next(c['summaries'][0] for c in store.snapshot('family-a')['children'] if c['summaries'])
    assert summary['status']=='fallback' and 'unavailable' in summary['recap']


def test_encryption_redaction_profile_edit_and_xss(system):
    store,c,a,_,_,w=system
    p=payload(store);p['turns'][0]['text']='I am Amy. a\100example.com 13812345678. My address is 12 Long Road.'
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==200
    assert w.once()
    auth=parent(c);data=c.get('/parent/data').json();text=data['records'][0]['transcript']['turns'][0]['text']
    assert 'example.com' not in text and '13812345678' not in text and 'Long Road' not in text
    profile=dict(name='Amy',aliases=['Amy','孩子'],grade='二年级',level='English',interests='<script>alert(1)</script>',goals='')
    assert c.put('/parent/profiles/'+p['child_id'],headers=auth,json=profile).status_code==200
    assert c.get('/device/snapshot',headers=headers(a)).json()['children']
    with store.db() as db:
        raw=db.execute('SELECT payload FROM profiles WHERE id=?',(p['child_id'],)).fetchone()[0]
        assert b'<script>' not in raw
        assert db.execute('PRAGMA journal_mode').fetchone()[0]=='wal'
        assert db.execute('PRAGMA user_version').fetchone()[0]==1
    js=c.get('/app.js').text
    assert 'innerHTML' not in js and 'textContent' in js
    assert "frame-ancestors 'none'" in c.get('/').headers['content-security-policy']
    assert 'Amy' not in data['records'][0]['summary']['recap']


def test_parent_auth_cookie_and_logout(system):
    _,c,_,_,_,_=system
    assert c.post('/parent/login',json={'household':'family-a','password':'x'}).status_code==403
    assert c.post('/parent/login',headers={'Origin':ORIGIN},json={'household':'family-a','password':'x'}).status_code==401
    auth=parent(c)
    assert c.get('/parent/data').status_code==200
    assert c.post('/parent/logout',headers=auth).status_code==200
    assert c.get('/parent/data').status_code==401


def test_body_audio_and_invalid_time_rejected(system):
    store,c,a,_,_,_=system
    p=payload(store);p['audio']='forbidden'
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==422
    p=payload(store);p['started_at']-=31*DAY
    assert c.post('/device/sessions',headers=headers(a),json=p).status_code==410
    assert c.post('/device/sessions',headers=headers(a),content=b'x'*(301*1024)).status_code==413


def test_backup_restore_reapplies_latest_deletion(system,tmp_path):
    from server.backup import backup,restore
    store,_,a,_,_,worker=system
    # Store AESGCM intentionally has no key-export API; create a fixture with known key.
    key=os.urandom(32);source=Store(tmp_path/'source.sqlite',key)
    secret=source.provision('family-a','long-backup-parent-password','backup-speaker', ['Amy','Ben'])
    source.upload(source.device(secret),Upload.model_validate(payload(source)))
    Worker(source).once()
    snapshot=backup(source,tmp_path/'backups')
    source.delete('family-a',session_id='session-a')
    restored=restore(snapshot,tmp_path/'restored.sqlite',key,source.path+'.deletions')
    assert restored.snapshot('family-a')['generation']==1
    assert restored.records('family-a')['records']==[]
    assert not Worker(restored).once()
    p=payload(restored);p['session_id']='session-a'
    assert restored.upload(restored.device(secret),Upload.model_validate(p))[0]==410
    with pytest.raises(ValueError): restore(snapshot,tmp_path/'other.sqlite',key,tmp_path/'missing.deletions')


def test_other_child_delete_requeues_inflight_job(system):
    store,_,a,_,_,_=system
    p=payload(store);store.upload(store.device(a),Upload.model_validate(p))
    ben=next(ch['id'] for ch in store.snapshot('family-a')['children'] if ch['profile']['name']=='Ben')
    class ChangesEpoch:
        name='mock-change'
        called=0
        def summarize(self,turns):
            self.called+=1
            if self.called==1: store.delete('family-a',child_id=ben)
            return dict(topic='dinosaurs',recap='Practice',resume_question='Continue?',new_words=[])
    model=ChangesEpoch();w=Worker(store,model)
    assert w.once();assert w.once();assert model.called==2
    assert any(ch['summaries'] for ch in store.snapshot('family-a')['children'])
