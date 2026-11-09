"""Local administrative commands; credentials written only to private files."""
import argparse
import getpass
import os
import secrets
import sys
from pathlib import Path
from .store import Store, digest
from .config import data_key


def ask_password(parser):
    if not sys.stdin.isatty():
        parser.error('interactive SSH terminal required; password input must not be echoed')
    password = getpass.getpass('家长密码（8–256字符，不回显）: ')
    if not 8 <= len(password) <= 256:
        parser.error('password must contain 8 to 256 characters')
    if password != getpass.getpass('再次输入: '):
        parser.error('passwords do not match')
    return password


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--child', action='append', default=[])
    parser.add_argument('command',choices=['provision','cleanup','revoke','rotate','set-parent-password','set-ark-key',
                                           'check-test-session','delete-test-session'])
    parser.add_argument('--household')
    parser.add_argument('--device')
    parser.add_argument('--secret-out',type=Path)
    parser.add_argument('--without-parent-password',action='store_true')
    parser.add_argument('--secret-stdout',action='store_true',help='private SSH pipe only; never log stdout')
    parser.add_argument('--session')
    args=parser.parse_args()
    os.umask(0o077)
    if args.command == 'set-ark-key':
        from .ark_setup import configure
        return configure()
    store=Store(os.environ['MEMORY_DB'],data_key())
    if args.command=='provision':
        if not args.household or not args.device or bool(args.secret_out)==args.secret_stdout:
            parser.error('household/device and exactly one secret output required')
        password=None if args.without_parent_password else ask_password(parser)
        if args.secret_stdout:
            if sys.stdout.isatty(): parser.error('secret stdout requires a private pipe')
            sys.stdout.write(store.provision(args.household,password,args.device,args.child))
            sys.stdout.flush()
            return
        # Reserve output before modifying the database, refusing overwrite/symlink.
        fd=os.open(args.secret_out,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        try:
            secret=store.provision(args.household,password,args.device,args.child)
            os.write(fd,secret.encode());os.fsync(fd)
        finally: os.close(fd)
        print('Provisioned; device secret written to the requested private file.')
    elif args.command=='rotate':
        if not args.device or not args.secret_out: parser.error('device/secret-out required')
        fd=os.open(args.secret_out,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        try:
            secret=secrets.token_hex(32)
            os.write(fd,secret.encode());os.fsync(fd)
            with store.db() as db:
                if not db.execute('UPDATE devices SET secret_hash=?,revoked=0 WHERE id=?',(digest(secret),args.device)).rowcount:
                    parser.error('unknown device')
        finally: os.close(fd)
        print('Rotated; install new secret privately before reconnecting the device.')
    elif args.command in ('check-test-session','delete-test-session'):
        if not args.household or not args.session or not args.session.startswith('p5b-validation-'):
            parser.error('only an explicit P5b validation session may be inspected/deleted')
        with store.db() as db:
            row=db.execute('SELECT * FROM sessions WHERE id=? AND household_id=?',
                           (args.session,args.household)).fetchone()
            if not row: parser.error('test session not found in household')
            if args.command=='check-test-session':
                summary=db.execute('SELECT status FROM summaries WHERE session_id=?',(args.session,)).fetchone()
                if not summary or summary['status']!='fallback': parser.error('fallback summary not ready')
                print('Independent database readback: fallback summary present.')
                return
        store.delete(args.household,session_id=args.session)
        with store.db() as db:
            row=db.execute('SELECT payload,deleted FROM sessions WHERE id=?',(args.session,)).fetchone()
            if row['payload'] is not None or row['deleted']!=1: parser.error('deletion verification failed')
            for table in ('jobs','summaries'):
                if db.execute(f'SELECT count(*) FROM {table} WHERE session_id=?',(args.session,)).fetchone()[0]:
                    parser.error('deletion verification failed')
        print('Test text/summary/jobs removed; text-free deletion tombstone retained.')
    elif args.command=='set-parent-password':
        if not args.household: parser.error('household required')
        store.set_parent_password(args.household,ask_password(parser))
        print('家长密码已设置；旧登录会话已失效。')
    elif args.command=='cleanup': store.cleanup();print('Retention cleanup complete.')
    else:
        if not args.device: parser.error('device required')
        with store.db() as db: db.execute('UPDATE devices SET revoked=1 WHERE id=?',(args.device,))
        print('Device credential revoked.')


if __name__=='__main__': main()
