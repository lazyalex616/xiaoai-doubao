"""Encrypted SQLite backup; restore always reapplies the latest deletion ledger."""
import argparse
import os
import secrets
import shutil
import sqlite3
import tempfile
import time
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from .store import DAY, Store
from .config import data_key


def backup(store, out):
    out=Path(out);out.mkdir(parents=True,exist_ok=True,mode=0o700)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        raw=Path(tmp)/'consistent.sqlite'
        with store.db() as src:
            dst=sqlite3.connect(raw)
            try: src.backup(dst)
            finally: dst.close()
        nonce=secrets.token_bytes(12)
        encrypted=nonce+store.aes.encrypt(nonce,raw.read_bytes(),b'xiaoai-backup-v1')
        name=out/(str(int(time.time()))+'-'+secrets.token_hex(4)+'.backup.enc')
        fd=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'wb') as f: f.write(encrypted);f.flush();os.fsync(f.fileno())
    for p in out.glob('*.backup.enc'):
        if p.stat().st_mtime < time.time()-7*DAY: p.unlink()
    # A separate latest ledger must survive beyond the seven-day backup window.
    ledger=Path(store.path+'.deletions')
    if ledger.exists(): shutil.copyfile(ledger,out/'latest.deletions');os.chmod(out/'latest.deletions',0o600)
    else: (out/'latest.deletions').touch(mode=0o600)
    return name


def restore(backup_file,target,key,ledger):
    target=Path(target)
    if target.exists(): raise ValueError('refuse to overwrite existing database')
    ledger=Path(ledger)
    if not ledger.is_file(): raise ValueError('latest deletion ledger required')
    encrypted=Path(backup_file).read_bytes()
    raw=AESGCM(key).decrypt(encrypted[:12],encrypted[12:],b'xiaoai-backup-v1')
    target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(raw);f.flush();os.fsync(f.fileno())
    shutil.copyfile(ledger,str(target)+'.deletions');os.chmod(str(target)+'.deletions',0o600)
    store=Store(target,key) # Applies ledger before any data/API can be served.
    store.cleanup()
    return store


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('command',choices=['backup','restore'])
    parser.add_argument('--out');parser.add_argument('--backup');parser.add_argument('--target');parser.add_argument('--ledger')
    args=parser.parse_args();os.umask(0o077)
    key=data_key()
    if args.command=='backup':
        if not args.out: parser.error('--out required')
        store=Store(os.environ['MEMORY_DB'],key);backup(store,args.out)
        print('Encrypted consistent backup complete; copy the latest ledger separately.')
    else:
        if not all([args.backup,args.target,args.ledger]): parser.error('--backup/--target/--ledger required')
        restore(args.backup,args.target,key,args.ledger);print('Restore complete with deletion ledger and retention applied; service remains stopped.')


if __name__=='__main__': main()
