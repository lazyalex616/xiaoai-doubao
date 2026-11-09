"""USER TERMINAL ONLY: separately encrypt the server data key; never print it."""
import getpass
import os
from pathlib import Path
import secrets
import sys

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt
from p5b import check_host, remote

if not sys.stdin.isatty():
    raise SystemExit('Run in your own terminal; interactive hidden backup passphrase required.')
check_host()
password=getpass.getpass('独立备份口令（至少8字符，不回显）: ')
if len(password)<8 or password!=getpass.getpass('再次输入: '):
    raise SystemExit('备份口令至少8字符，且两次输入须一致。')
key=remote('sudo cat /etc/xiaoai-memory/data.key',capture=True)
if len(bytes.fromhex(key.decode().strip()))!=32:
    raise SystemExit('Invalid data key; no backup written.')
salt,nonce=secrets.token_bytes(16),secrets.token_bytes(12)
wrapping=Scrypt(salt=salt,length=32,n=2**17,r=8,p=1).derive(password.encode())
encrypted=b'XAMKEY1'+salt+nonce+AESGCM(wrapping).encrypt(nonce,key,b'xiaoai-key-backup-v1')
target=Path.home()/'.config/xiaoai/memory-data-key.backup.enc'
target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
with os.fdopen(fd,'wb') as f:
    f.write(encrypted);f.flush();os.fsync(f.fileno())
print('Independent encrypted data-key backup created (0600). Keep its passphrase separately.')
