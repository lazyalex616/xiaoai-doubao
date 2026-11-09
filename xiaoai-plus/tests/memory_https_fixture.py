"""Local TLS regression: delayed handshake/body, CA/hostname, JSON/framing, log whitelist."""
import json, os, pathlib, socket, ssl, subprocess, sys, tempfile, threading, time
with tempfile.TemporaryDirectory(prefix='xiaoai-https-') as temp:
    root=pathlib.Path(temp);cert=root/'cert.pem';key=root/'key.pem'
    (root/'req.cnf').write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\n[dn]\n[ext]\nsubjectAltName=DNS:localhost\n')
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
        '-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost',
        '-config',str(root/'req.cnf')],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cert,key)
    cases=['legacy_slow','slow','chunked','bad_json','bad_length','bad_ca','wrong_host']
    if sys.platform.startswith('linux'):cases.insert(2,'slow_dns') # GNU --wrap only.
    for case in cases:
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(8)
        port=listener.getsockname()[1]
        def serve():
            try:
                raw,_=listener.accept()
                if case in ('legacy_slow','slow'):time.sleep(1.2)
                with ctx.wrap_socket(raw,server_side=True) as conn:
                    request=b''
                    while b'\r\n\r\n' not in request:request+=conn.recv(4096)
                    if case in ('legacy_slow','slow'):time.sleep(1.2)
                    body=b'{"status":"ok","private":"PROFILE_SENTINEL"}'
                    if case=='bad_json':body=b'PROFILE_SENTINEL_not_json'
                    if case=='chunked':
                        response=b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'+hex(len(body))[2:].encode()+b'\r\n'+body+b'\r\n0\r\n\r\n'
                    else:
                        size=len(body)+(1 if case=='bad_length' else 0)
                        response=b'HTTP/1.1 200 OK\r\nContent-Length: '+str(size).encode()+b'\r\nConnection: close\r\n\r\n'+body
                    conn.sendall(response)
            except (ssl.SSLError,TimeoutError,OSError):pass
            finally:listener.close()
        thread=threading.Thread(target=serve);thread.start()
        ca=str(root/'missing.pem') if case=='bad_ca' else str(cert)
        host='127.0.0.1' if case=='wrong_host' else 'localhost'
        if case=='legacy_slow':
            config=root/'memory.ini';config.write_text('[memory]\nenabled = on\nserver = https://localhost:'+str(port)+'\ndevice_secret = '+'a'*64+'\n');config.chmod(0o600)
            # The probe uses the same relative CA path as the old resident config.
            assets=root/'assets';assets.mkdir(exist_ok=True);(assets/'ca-certificates.crt').write_bytes(cert.read_bytes())
            result=subprocess.run([sys.argv[2],str(config)],cwd=root,capture_output=True,text=True,timeout=20)
            assert 'category=connect_timeout' in result.stdout and 'http_status=0' in result.stdout,result
        else:
            env=os.environ.copy()
            if case=='slow_dns':env['P5C4_TEST_DNS_DELAY_MS']='5300'
            result=subprocess.run([sys.argv[1],f'https://{host}:{port}',ca,case],env=env,capture_output=True,text=True,timeout=20)
        thread.join(10)
        assert result.returncode==0,(case,result.stdout,result.stderr)
        assert 'PROFILE_SENTINEL' not in result.stderr and 'a'*64 not in result.stderr
        for line in result.stderr.splitlines():
            import re
            assert re.fullmatch(r'MEMORY_HTTPS phase=[a-z_]+ ms=\d+ category=[a-z_]+ code=-?\d+',line),line
        print(result.stdout.strip())
