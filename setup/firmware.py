"""OH2P offline preparation and single-device USB operations.
Derived from firmware-prep verify/layout/build, usb_window_probe and *_once tools.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
from usb_registry import classified, parse_ioreg

ROOT = Path(__file__).resolve().parent.parent
LOCAL = ROOT / '.local'
OTA_URL = 'https://cdn.cnbj1.fds.api.mi-img.com/xiaoqiang/rom/oh2p/mico_all_616cd9d93_1.62.2.bin'
OTA_SHA = '08c6c0c5e7dfd3de59a7e37b4709ad90ab84e8025c799a642730e97ef53c0eba'
TOOL_URL = 'https://raw.githubusercontent.com/khadas/utils/0dee958a4c3c53c0546ef98758ed721183f11a5f/aml-flash-tool/tools/macos/update'
TOOL_SHA = '19f37cc129d061512c24bffbbad3a51355d976c12d6091366fe7dfce3c949b30'
NEXT = {1:'bash setup/02-get-firmware.sh',2:'bash setup/03-make-patch.sh',3:'bash setup/04-get-flash-tool.sh',4:'连接原装12V电源和数据Type-C线，然后 python3 setup/05-usb-connect.py --execute',5:'bash setup/06-backup.sh --execute',6:'刷写用 bash setup/07-flash.sh --i-accept-brick-risk；只读彩排用 --reset-only --execute',7:'在米家App抄下MAC，运行 bash setup/08-find-speaker.sh <MAC>'}
DESC = {1:'检查Apple芯片Mac和依赖；用户按打印的brew命令自行安装。',2:'下载并核对OH2P 1.62.2，解包原厂rootfs；用户保持联网。',3:'生成专用RSA4096密钥，以公钥制作仅修改5个路径的镜像；不连接设备。',4:'下载固定Khadas update并取arm64，链接Homebrew libusb-compat；不访问USB。',5:'捕获同端口刷机窗口；先接电源和数据线，出现READY_TO_PLUG后拔DC立即插回；默认仅打印命令，--execute才identify。',6:'校准boot_part并双读boot0/boot1/system0/system1；用户保持电源和USB不动；--execute才读取。',7:'仅刷一次system0并读回，相同才reset；用户保持电源和USB不动；--i-accept-brick-risk才写；--reset-only --execute只reset。'}

class StatusParser(argparse.ArgumentParser):
    def error(self, message):
        print('STOP: invalid arguments\nNEXT: Run this script with --help')
        self.exit(2)


def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''): h.update(b)
    return h.hexdigest()

def run(cmd, timeout=180):
    p=subprocess.run([str(x) for x in cmd],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
    if p.returncode: raise RuntimeError('command failed: '+str(cmd[0]))
    return p.stdout.decode(errors='replace')

def download(url, p, digest):
    if not p.exists():
        run(['curl','--fail','--location','--silent','--show-error',url,'--output',p])
    p.chmod(0o600)
    if sha(p)!=digest: raise RuntimeError('download SHA-256 mismatch: '+p.name)

def save(name, value):
    p=LOCAL/name;p.write_text(json.dumps(value));p.chmod(0o600)

def registry(args):
    return classified(parse_ioreg(run([args.ioreg,'-p','IOUSB','-w0','-l','-r','-c','IOUSBHostDevice'],3)))

def snapshot(args, port):
    g=registry(args)
    if len(g['worldcup'])!=1 or g['worldcup'][0]['location']!=port or g['normal'] or g['other_amlogic']:
        raise RuntimeError('WorldCup must be unique at baseline port; GitHub提Issue')

def tool(args, *cmd, failed_test=False):
    port=json.loads((LOCAL/'usb.json').read_text())['port'];snapshot(args,port)
    p=subprocess.run([str(args.update),*map(str,cmd)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    text=p.stdout.decode(errors='replace')
    # Raw tool output may contain device identity; retain it only in private local log.
    with (LOCAL/'update.log').open('ab') as f: f.write(p.stdout)
    snapshot(args,port)
    if failed_test: return p.returncode,text
    if p.returncode or any(x in text.lower() for x in ('err','fail','timeout')): raise RuntimeError('update failed; no retry, no reset')
    return text

def reset(a):
    port=json.loads((LOCAL/'usb.json').read_text())['port'];snapshot(a,port)
    with (LOCAL/'update.log').open('ab') as log:
        subprocess.run([str(a.update),'bulkcmd','     reset'],stdout=log,
                       stderr=subprocess.STDOUT,timeout=20)
    end=time.monotonic()+60
    while time.monotonic()<end:
        g=registry(a)
        devices=g['normal']+g['worldcup']
        if (g['other_amlogic'] or len(devices)>1 or
                any(d['location']!=port for d in devices)):
            break
        if len(g['normal'])==1 and not g['worldcup']:
            return
        time.sleep(.2)
    raise RuntimeError('音箱没有正常回到普通模式；先不要拔插，到 GitHub 提 Issue')

def boot_part(a):
    results=[]
    for cmd in ('test 1 = 1','test 1 = 0','test ${boot_part} = boot0','test ${boot_part} = boot1'):
        rc,t=tool(a,'bulkcmd','     '+cmd,failed_test=True)
        ok=rc==0 and 'bulkInReply success' in t and 'failed' not in t.lower()
        bad='failed' in t.lower() and not ok
        if not(ok or bad): raise RuntimeError('unknown U-Boot test status')
        results.append(ok)
    if results[:2]!=[True,False] or results[2: ] not in ([True,False],[False,True]): raise RuntimeError('boot_part calibration failed')
    return 'boot0' if results[2] else 'boot1'

def read(a, part, size, dest):
    if dest.exists(): raise RuntimeError('output exists: '+dest.name)
    tool(a,'mread','store',part,'normal',hex(size),dest)
    dest.chmod(0o600)
    if dest.stat().st_size!=size: raise RuntimeError('read length mismatch')

def check_stock():
    b=LOCAL/'backup';ota=LOCAL/'extracted'
    if sha(b/'boot0.1.bin')!=sha(ota/'boot.img.encrypt'): raise RuntimeError('boot0 differs from OTA; GitHub提Issue')
    stock=(ota/'root.squashfs').read_bytes()
    with (b/'system0.1.bin').open('rb') as f:
        if f.read(len(stock))!=stock: raise RuntimeError('system0 prefix differs from OTA; GitHub提Issue')

def prepare(step,a):
    if step==2:
        from validate_ota_layout import main as layout
        from verify_ota import digest_file, EXPECTED_MD5
        ota=LOCAL/OTA_URL.rsplit('/',1)[-1];download(OTA_URL,ota,OTA_SHA)
        if digest_file(ota)[1]!=EXPECTED_MD5: raise RuntimeError('OTA MD5 mismatch')
        # Reuse the original boundary validator; extract those validated segments directly.
        import contextlib,io
        buf=io.StringIO()
        with contextlib.redirect_stdout(buf): layout(ota)
        info=json.loads(buf.getvalue());save('ota-layout.json',info)
        extractor=LOCAL/'extract.py'
        download('https://raw.githubusercontent.com/stevenjoezhang/xiaoai-agent/352661832dc609da15839cfde82ea6982f91787e/deploy/client-patch/src/extract.py',extractor,'417094a85658cbe8139ea96b4f657f7297d731144c68b696dcf83752c9576369')
        run([sys.executable,extractor,'-s',ota])
        ex=LOCAL/'extracted';ex.mkdir(exist_ok=True,mode=0o700)
        with ota.open('rb') as f:
            for seg in info['segments']:
                f.seek(seg['payload_offset']);p=ex/seg['name'];p.write_bytes(f.read(seg['payload_bytes']));p.chmod(0o600)
        tree=LOCAL/'rootfs-1.62.2'
        if tree.exists(): raise RuntimeError('rootfs already exists; use existing result or move .local/rootfs-1.62.2 aside')
        exclude=LOCAL/'exclude.txt';exclude.write_text('dev/console\n')
        run([a.unsquashfs,'-exclude-file',exclude,'-d',tree,ex/'root.squashfs'])
        v=(tree/'usr/share/mico/version').read_text()
        if 'OH2P' not in v or '1.62.2' not in v: raise RuntimeError('rootfs identity mismatch')
    elif step==3:
        key=a.key.expanduser();key.parent.mkdir(exist_ok=True,mode=0o700)
        if not key.exists(): run(['ssh-keygen','-q','-t','rsa','-b','4096','-N','','-f',key])
        key.chmod(0o600)
        if Path(str(key)+'.pub').exists(): Path(str(key)+'.pub').chmod(0o600)
        pub=LOCAL/'patch.pub';pub.write_text(run(['ssh-keygen','-y','-f',key]));pub.chmod(0o600)
        if not run(['ssh-keygen','-lf',pub]).startswith('4096 '): raise RuntimeError('dedicated key must be RSA4096')
        run([sys.executable,ROOT/'setup/build_minimal_patch.py','--public-key',pub,'--tree',LOCAL/'patched-tree','--image',LOCAL/'root-patched.squashfs','--packer',a.mksquashfs])
        (LOCAL/'root-patched.squashfs').chmod(0o600)
        save('patch.json',{'sha256':sha(LOCAL/'root-patched.squashfs')})
    elif step==4:
        vendor=LOCAL/'update.vendor';download(TOOL_URL,vendor,TOOL_SHA)
        out=LOCAL/'update';run(['lipo','-thin','arm64',vendor,'-output',out])
        lib=a.libusb or Path(run(['brew','--prefix','libusb-compat']).strip())/'lib/libusb-0.1.4.dylib'
        run(['install_name_tool','-change','/opt/homebrew/opt/libusb-compat/lib/libusb-0.1.4.dylib',lib,out]);run(['codesign','--force','--sign','-',out]);out.chmod(0o700)
        save('tool.json',{'sha256':sha(out)})
    elif step==5:
        g=registry(a)
        if len(g['normal'])!=1 or g['worldcup'] or g['other_amlogic']: raise RuntimeError('baseline must have exactly one normal OH2P; GitHub提Issue')
        port=g['normal'][0]['location']
        print('READY_TO_PLUG: 现在拔掉DC电源并立即插回，Type-C保持连接，人不用抢窗口。',flush=True)
        end=time.monotonic()+a.duration
        while time.monotonic()<end:
            g=registry(a)
            if len(g['normal'])>1 or g['other_amlogic'] or len(g['worldcup'])>1: raise RuntimeError('USB device ambiguity; GitHub提Issue')
            if g['worldcup']:
                snapshot(a,port)
                # Persist only after identify, keeping filesystem I/O out of the short window.
                p=subprocess.run([str(a.update),'identify','7'],capture_output=True,timeout=8)
                if p.returncode or b'This firmware version is' not in p.stdout or b'ERR' in p.stdout or b'fail' in p.stdout.lower(): raise RuntimeError('identify failed; no retry')
                snapshot(a,port);save('usb.json',{'port':port});return
            if g['normal'] and g['normal'][0]['location']!=port: raise RuntimeError('normal USB changed port; GitHub提Issue')
            time.sleep(.1)
        raise RuntimeError('USB window timed out')
    elif step==6:
        part=boot_part(a);save('boot-part.json',{'boot_part':part})
        b=LOCAL/'backup';b.mkdir(mode=0o700,exist_ok=True)
        for part in ('boot0','boot1','system0','system1'):
            # Derive boot image length from Android header, retaining its 512-byte trailer.
            if part.startswith('boot'):
                head=b/(part+'.head');read(a,part,4096,head);data=head.read_bytes()
                if data[:8]!=b'ANDROID!': raise RuntimeError('boot header magic mismatch')
                ks,ka,rs,ra,ss,sa,ta,pg,dt,un=struct.unpack_from('<10I',data,8)
                if pg!=2048 or max(ks,rs,ss,dt)>10000000: raise RuntimeError('boot header size mismatch')
                size=pg+sum((x+pg-1)//pg*pg for x in (ks,rs,ss,dt))+512
            else: size=0x02800000
            for i in (1,2): read(a,part,size,b/f'{part}.{i}.bin')
            if part.startswith('boot') and (b/f'{part}.1.bin').read_bytes()[:4096]!=head.read_bytes()[:min(size,4096)]: raise RuntimeError('boot header changed')
            if sha(b/f'{part}.1.bin')!=sha(b/f'{part}.2.bin'): raise RuntimeError('backup hash mismatch; GitHub提Issue')
        check_stock();save('backup.json',{'files':{p.name:sha(p) for p in b.glob('*.bin')}})
    elif step==7:
        if a.reset_only: reset(a);return
        if boot_part(a)!='boot0': raise RuntimeError('boot_part=boot1 unsupported; GitHub提Issue')
        check_stock();manifest=json.loads((LOCAL/'backup.json').read_text())
        for name,digest in manifest['files'].items():
            if sha(LOCAL/'backup'/name)!=digest: raise RuntimeError('backup changed')
        image=LOCAL/'root-patched.squashfs';size=image.stat().st_size
        if sha(image)!=json.loads((LOCAL/'patch.json').read_text())['sha256']: raise RuntimeError('patch hash changed')
        if not 0<size<0x02800000: raise RuntimeError('patch length out of range')
        tool(a,'partition','system0',image)
        dest=LOCAL/'flash-readback.bin';read(a,'system0',size,dest)
        if sha(dest)!=sha(image): raise RuntimeError('readback hash mismatch; no reset; GitHub提Issue')
        reset(a)

def entry(step=None):
    os.umask(0o077)
    step=step or int(sys.argv.pop(1))
    p=StatusParser(description=DESC[step],epilog='OK: help complete\nNEXT: 按本步说明操作；日志和产物在.local/。',formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--execute',action='store_true',help='明确允许本步USB命令；默认不接触设备')
    p.add_argument('--i-accept-brick-risk',action='store_true',help='接受刷坏风险，允许单次system0写入')
    p.add_argument('--reset-only',action='store_true',help='配合--execute只发送一次reset，不写分区')
    p.add_argument('--key',type=Path,default=Path('~/.ssh/xiaoai_doubao_rsa'),help='制作补丁用专用RSA4096私钥路径')
    p.add_argument('--unsquashfs',default=shutil.which('unsquashfs') or 'unsquashfs')
    p.add_argument('--mksquashfs',default=shutil.which('mksquashfs') or 'mksquashfs')
    p.add_argument('--libusb',type=Path,help='本机libusb-compat dylib；默认Homebrew')
    p.add_argument('--ioreg',default='/usr/sbin/ioreg',help='ioreg路径；离线模拟可替换')
    p.add_argument('--update',type=Path,default=LOCAL/'update',help='update路径；离线模拟可替换')
    p.add_argument('--duration',type=float,default=120)
    a=p.parse_args()
    if not 3<=a.duration<=120: p.error('duration must be 3..120')
    if step>=5 and not (a.i_accept_brick_risk if step==7 and not a.reset_only else a.execute):
        print({5:'update identify 7',6:'update bulkcmd test; update mread store <boot0/boot1/system0/system1> normal <length> <file> (twice)',7:'update bulkcmd "     reset"' if a.reset_only else 'update partition system0 .local/root-patched.squashfs; update mread ...; update bulkcmd "     reset"'}[step])
        print('OK: dry run; no device commands\nNEXT: '+NEXT[step]);return
    LOCAL.mkdir(mode=0o700,exist_ok=True)
    try:
        if step>=5:
            pin=json.loads((LOCAL/'tool.json').read_text())['sha256']
            if sha(a.update)!=pin: raise RuntimeError('local update SHA-256 mismatch')
        prepare(step,a)
        print('OK: step '+str(step)+' complete\nNEXT: '+NEXT[step])
    except Exception as e:
        print('STOP: '+str(e)+'\nNEXT: 停止，不重试；检查原因，未覆盖情形请到GitHub提Issue');sys.exit(1)

if __name__=='__main__':
    if len(sys.argv)==1 or sys.argv[1] in ('-h','--help'):
        print('Internal helper: use the numbered setup scripts with --help.\nOK: help complete\nNEXT: Run setup/01-check-mac.sh --help')
    else: entry()
