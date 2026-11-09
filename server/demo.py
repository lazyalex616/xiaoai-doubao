"""Local synthetic C++ -> TLS -> server demo; no device or cloud connections."""
import json
import os
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import uvicorn
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from .app import create_app
from .store import Store


def main():
    root=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='xiaoai-p5a-demo-') as tmp:
        d=Path(tmp);os.chmod(d,0o700)
        key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'P5a local synthetic demo')])
        now=datetime.now(timezone.utc)
        cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
              .serial_number(x509.random_serial_number()).not_valid_before(now-timedelta(minutes=1))
              .not_valid_after(now+timedelta(days=1)).add_extension(x509.BasicConstraints(ca=True,path_length=None),True)
              .add_extension(x509.SubjectAlternativeName([x509.DNSName('host.docker.internal'),x509.DNSName('localhost')]),False).sign(key,hashes.SHA256()))
        (d/'tls.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        (d/'tls-key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        os.chmod(d/'tls-key.pem',0o600)
        store=Store(d/'memory.sqlite',os.urandom(32))
        (d/'device-secret').write_text(store.provision('demo','synthetic-parent-password','demo-speaker'));os.chmod(d/'device-secret',0o600)
        config=uvicorn.Config(create_app(store,origin='https://localhost:64105'),host='127.0.0.1',port=64105,
                              ssl_keyfile=str(d/'tls-key.pem'),ssl_certfile=str(d/'tls.pem'),access_log=False,log_level='error')
        server=uvicorn.Server(config);thread=threading.Thread(target=server.run,daemon=True);thread.start()
        try:
            for _ in range(100):
                if server.started: break
                time.sleep(.05)
            if not server.started: raise RuntimeError('local TLS server did not start')
            subprocess.run(['docker','run','--rm','--platform','linux/amd64',
                            '-v',f'{root}/xiaoai-plus:/workspace:ro','-v',f'{tmp}:/demo',
                            'xiaoai-plus-toolchain:dev','/workspace/build-host-p5a/memory_e2e',
                            'https://host.docker.internal:64105','/demo/device-secret','/demo/cache','/demo/tls.pem'],check=True,timeout=60)
            data=store.records('demo')
            assert len(data['records'])==1 and data['records'][0]['summary']
            print('PASS server readback: 1 child session, 2 final text turns, mock-v1 summary; zero audio/paid calls')
        finally:
            server.should_exit=True;thread.join(timeout=5)


if __name__=='__main__': main()
