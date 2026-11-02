#!/usr/bin/env python3
"""Read-only wake diagnostics. No DPMS writes, KMS commits or GPU power queries."""
import argparse,ctypes as C,datetime,json,logging,logging.handlers,os,socket,subprocess,sys,time,shlex
from pathlib import Path
ROOT=Path.home()/'.local/state/hypr-output-wake'
RUNTIME=Path(os.environ.get('XDG_RUNTIME_DIR',f'/run/user/{os.getuid()}'))/'hypr'

def request(command,path=None):
    paths=[Path(path)] if path else sorted(RUNTIME.glob('*/.socket.sock'),key=lambda p:p.stat().st_mtime,reverse=True)
    error='No compositor socket'
    for target in paths:
        try:
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
                s.settimeout(2);s.connect(str(target));s.sendall(('/'+command).encode());data=bytearray()
                while True:
                    block=s.recv(65536)
                    if not block:break
                    data.extend(block)
                    if len(data)>8*1024*1024:raise RuntimeError('Oversized IPC response')
            return data.decode(errors='replace'),str(target)
        except (OSError,RuntimeError) as e:error=str(e)
    raise RuntimeError(error)

def probe():
    text,path=request('wakeprobe status')
    return json.loads(text),path

def backlights():
    result={}
    for p in Path('/sys/class/backlight').glob('*'):
        fields={}
        for key in ['brightness','actual_brightness','max_brightness','bl_power']:
            try:fields[key]=int((p/key).read_text())
            except (OSError,ValueError):fields[key]=None
        result[p.name]=fields
    return result

def lid():
    result=[]
    for p in Path('/proc/acpi/button/lid').glob('*/state'):
        try:result.append(p.read_text().strip())
        except OSError:pass
    return result

def mismatch(state,lights,lids):
    if not state.get('global_dpms') or any('closed' in x for x in lids):return False
    panel=any(x['monitor'].get('name','').startswith('eDP') and x['monitor'].get('enabled') and x['monitor'].get('dpms') for x in state.get('monitors',[]))
    return panel and any((x.get('brightness') or 0)>0 and (x.get('bl_power') not in (None,0) or x.get('actual_brightness')==0) for x in lights.values())

class Props(C.Structure):
    _fields_=[('count',C.c_uint32),('ids',C.POINTER(C.c_uint32)),('values',C.POINTER(C.c_uint64))]
class Property(C.Structure):
    _fields_=[('id',C.c_uint32),('flags',C.c_uint32),('name',C.c_char*32),('count_values',C.c_int),('values',C.POINTER(C.c_uint64)),('count_enums',C.c_int),('enums',C.c_void_p),('count_blobs',C.c_int),('blob_ids',C.POINTER(C.c_uint32))]

def kernel(state):
    lib=C.CDLL('libdrm.so.2',use_errno=True)
    lib.drmSetClientCap.argtypes=[C.c_int,C.c_uint64,C.c_uint64]
    lib.drmModeObjectGetProperties.argtypes=[C.c_int,C.c_uint32,C.c_uint32];lib.drmModeObjectGetProperties.restype=C.POINTER(Props)
    lib.drmModeFreeObjectProperties.argtypes=[C.POINTER(Props)]
    lib.drmModeGetProperty.argtypes=[C.c_int,C.c_uint32];lib.drmModeGetProperty.restype=C.POINTER(Property)
    lib.drmModeFreeProperty.argtypes=[C.POINTER(Property)]
    def properties(fd,object_id,kind):
        p=lib.drmModeObjectGetProperties(fd,object_id,kind)
        if not p:return {'error':os.strerror(C.get_errno())}
        result={}
        try:
            for i in range(p.contents.count):
                prop=lib.drmModeGetProperty(fd,p.contents.ids[i])
                if not prop:continue
                try:
                    name=bytes(prop.contents.name).decode(errors='replace')
                    if name in ['CRTC_ID','ACTIVE','MODE_ID','FB_ID','DPMS','link-status']:result[name]=int(p.contents.values[i])
                finally:lib.drmModeFreeProperty(prop)
        finally:lib.drmModeFreeObjectProperties(p)
        return result
    result=[]
    for row in state.get('monitors',[]):
        b=row['backend'];pci=b.get('pci','');name=row['monitor']['name']
        item={'monitor':name,'pci':pci,'cached_crtc':b.get('cached_crtc')}
        try:
            if not pci or (Path('/sys/bus/pci/devices')/pci/'vendor').read_text().strip()!='0x8086':
                item['skipped']='Kernel queries restricted to Intel; other outputs use cached probe state';result.append(item);continue
            node=Path('/dev/dri/by-path')/f'pci-{pci}-card'
            fd=os.open(node,os.O_RDONLY|os.O_CLOEXEC)
            try:
                lib.drmSetClientCap(fd,3,1) # Per-FD atomic property visibility, no KMS state changes.
                item['connector']=properties(fd,int(b['connector_id']),0xc0c0c0c0)
                crtc=item['connector'].get('CRTC_ID',0)
                if crtc:item['crtc']=properties(fd,crtc,0xcccccccc)
                if b.get('cached_crtc') and b['cached_crtc']!=crtc:item['cached_crtc_state']=properties(fd,b['cached_crtc'],0xcccccccc)
            finally:os.close(fd)
        except (OSError,KeyError,ValueError) as e:item['error']=str(e)
        result.append(item)
    return result

def isolated_kernel(state):
    command=[sys.executable,str(Path(__file__).resolve()),'--kernel-input']
    with (Path.home()/'codex.log').open('a') as log:
        log.write(f'\n[{datetime.datetime.now().astimezone().isoformat()}] cwd={os.getcwd()} command={shlex.join(command)}\n');log.flush()
        try:
            p=subprocess.run(command,input=json.dumps(state),text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=3)
            log.write(p.stdout+p.stderr+f'[exit_status={p.returncode}]\n')
            return json.loads(p.stdout) if p.returncode==0 else {'error':p.stderr[-2000:]}
        except (subprocess.TimeoutExpired,ValueError) as e:
            log.write(f'{e}\n[exit_status=timeout-or-invalid-json]\n');return {'error':str(e)}

def snapshot(label,state=None,path=None):
    ROOT.mkdir(parents=True,exist_ok=True)
    report={'time':datetime.datetime.now().astimezone().isoformat(),'label':label,'backlights':backlights(),'lid':lid()}
    try:
        if state is None:state,path=probe()
        report['probe']=state;report['socket']=path;report['kernel']=isolated_kernel(state)
        for cmd in ['gpu-switch','gpu-switch transfers','gpu-switch rendering','rollinglog','configerrors']:
            try:report[cmd]=request(cmd,path)[0]
            except RuntimeError as e:report[cmd]={'error':str(e)}
    except (RuntimeError,ValueError) as e:report['error']=str(e)
    destination=ROOT/('snapshot-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')+'.json')
    with destination.open('x') as f:os.chmod(destination,0o600);json.dump(report,f,ensure_ascii=False,indent=2)
    files=sorted(ROOT.glob('snapshot-*.json'))
    for old in files[:-40]:old.unlink()
    return destination

def watch():
    ROOT.mkdir(parents=True,exist_ok=True)
    log=logging.getLogger('wake');log.setLevel(logging.INFO)
    handler=logging.handlers.RotatingFileHandler(ROOT/'events.jsonl',maxBytes=4*1024*1024,backupCount=3);log.addHandler(handler)
    def record(kind,**data):log.info(json.dumps({'time':datetime.datetime.now().astimezone().isoformat(),'kind':kind,**data},ensure_ascii=False))
    last=None;sequence=0;due=[];bad_since=None;reported=False;heartbeat=0;failed=False
    record('watch-start',pid=os.getpid())
    while True:
        try:
            state,path=probe();lights=backlights();lids=lid();now=time.monotonic()
            identity=(path,state['generation'])
            if identity!=last:
                record('probe-start',state=state,backlights=lights);last=identity;sequence=0;due=[now+1];failed=False
            new=[e for e in state['events'] if e['sequence']>sequence]
            if new:
                sequence=state['sequence'];record('probe-events',events=new,state=state,backlights=lights,lid=lids)
                if any(e['kind'] in ['dpms-intent-before-commit','monitor-removed','monitor-watch'] for e in new):due=[now+x for x in [.25,1,3,10]];bad_since=None;reported=False
            if due and now>=due[0]:
                due.pop(0);record('settling',state=state,backlights=lights,lid=lids)
                if not due:
                    file=snapshot('after-dpms-or-monitor-event',state,path);record('snapshot',path=str(file))
            bad=mismatch(state,lights,lids)
            if bad:
                if bad_since is None:bad_since=now
                if now-bad_since>=2 and not reported:
                    file=snapshot('panel-dpms-on-backlight-off',state,path);record('anomaly',path=str(file),state=state,backlights=lights);reported=True
            else:bad_since=None;reported=False
            if now-heartbeat>=60:record('heartbeat',state=state,backlights=lights,lid=lids);heartbeat=now
            time.sleep(.25 if due else 1)
        except (RuntimeError,ValueError,OSError,KeyError) as e:
            if not failed:record('probe-unavailable',error=str(e));failed=True
            time.sleep(2)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--watch',action='store_true');parser.add_argument('--kernel-input',action='store_true');parser.add_argument('--snapshot',nargs='?',const='manual')
    args=parser.parse_args()
    if args.kernel_input:print(json.dumps(kernel(json.load(sys.stdin))));return
    if args.watch:watch();return
    print(snapshot(args.snapshot or 'manual'))
if __name__=='__main__':main()
