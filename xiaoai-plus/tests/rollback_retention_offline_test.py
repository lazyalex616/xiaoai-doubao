"""Run actual retention logic against private local fixtures, including refusal."""
import gzip
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('retention',ROOT/'toolchain/rollback-retention.py');r=importlib.util.module_from_spec(s);s.loader.exec_module(r)
real=subprocess.check_output
for unsafe in (False,True):
 with tempfile.TemporaryDirectory() as tmp:
  app=Path(tmp)/'app';app.mkdir();logs=Path(tmp)/'logs';logs.mkdir()
  old=b'memory ELF fixture';stable=b'stable ELF fixture'
  for n,data in [('xiaoai_plus_speaker',old),('boot.sh',b'boot fixture'),('config.ini',b'config fixture'),('memory-device.secret',b'fake offline secret')]:
   (app/n).write_bytes(data);(app/n).chmod(0o600)
  for name,data in [('rollback-old',b'obsolete'),('rollback-stable',stable),('rollback-stable-duplicate',stable)]:
   d=app/name;d.mkdir();(d/'xiaoai_plus_speaker').write_bytes(data);(d/'boot.sh').write_bytes(b'boot')
  if unsafe:(app/'rollback-old'/'unsafe').symlink_to(app/'config.ini')
  before={n:(app/n).read_bytes() for n in ['xiaoai_plus_speaker','boot.sh','config.ini','memory-device.secret']}
  pins={'P5F2_TRANSACTION':'p5f2-20261003','P5F2_PREDECESSOR_SHA256':hashlib.sha256(old).hexdigest(),'P5F2_CONFIG_SHA256':hashlib.sha256(before['config.ini']).hexdigest(),'P5F2_STABLE_SHA256':hashlib.sha256(stable).hexdigest()}
  def fixture(cmd,*,input):
   body=input.decode().replace('/data/xiaoai-plus',str(app)).replace('df -k /data','df -k '+str(app))
   # sha256sum provided by a local wrapper on macOS.
   return real(['/bin/sh','-s'],input=body.encode(),env=env)
  import os
  wrapper=Path(tmp)/'sha256sum';wrapper.write_text('#!/bin/sh\nexec shasum -a 256 "$@"\n');wrapper.chmod(0o755)
  env=dict(os.environ,PATH=tmp+':'+os.environ['PATH'])
  r.subprocess.check_output=fixture
  try:
   r.retain([],pins,logs)
   assert not unsafe
   assert len(list(app.glob('rollback-*')))==2
   assert gzip.decompress((app/'rollback-p5f2-20261003/xiaoai_plus_speaker.gz').read_bytes())==old
   assert gzip.decompress((app/'rollback-stable/xiaoai_plus_speaker.gz').read_bytes())==stable
   assert 'rollback-old/xiaoai_plus_speaker' in (logs/'deleted-manifest.txt').read_text()
  except subprocess.CalledProcessError:
   assert unsafe
   assert not (logs/'deleted-manifest.txt').exists()
   assert (app/'rollback-old/xiaoai_plus_speaker').read_bytes()==b'obsolete'
  finally:r.subprocess.check_output=real
  assert before=={n:(app/n).read_bytes() for n in before}
print('Retention: two compressed verified rollbacks, exact audit/deletion, resident preserved, symlink refusal PASS')
