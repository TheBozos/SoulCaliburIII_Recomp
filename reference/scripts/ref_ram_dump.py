#!/usr/bin/env python3
# usage: ref_dump.py <out dir> <env file> <counters...>: runs the reference runner, stops it when RAM word 0x46e608 reaches each counter and writes RAM to <out>/ram_<n>.bin.
import os,re,signal,struct,subprocess,sys,time
out,envf=sys.argv[1],sys.argv[2]; marks=sorted(int(x) for x in sys.argv[3:])
env=dict(os.environ)
os.makedirs(out,exist_ok=True)
user=out+'/user'; os.makedirs(user,exist_ok=True)
B=os.path.expanduser('~/recomp/sc3/bozo-b1')
elf=os.environ['REF_ELF']
open(user+'/migrated.txt','w').write(os.path.dirname(elf)+'\n')
env.update(PS2X_USER_DIR=user,PS2X_NO_DIALOGS='1',PS2X_WINDOW_NO_FOCUS='1',PS2X_PAD_NO_HOST='1',PS2X_DEBUG_UI='0',PS2X_LOG_FILE='0',
  SC3_TEXTURE_PACK=os.environ['REF_PACKS']+'/sc3_textures.pack',SC3_2D_PACK=os.environ['REF_PACKS']+'/sc3_2d.pack',SC3_CALL_NAMES=B+'/sc3_call_names.txt')
here=os.path.dirname(os.path.abspath(envf))
for line in open(envf):
    line=line.strip()
    if not line or '=' not in line: continue
    k,v=line.split('=',1)
    if v.startswith('@'): v=open(here+'/'+v[1:]).read().strip()
    if k in('PS2X_CALL_LOG','PS2X_CAPTURE_WINDOW_DIR','PS2X_CAPTURE_START','PS2X_CAPTURE_INTERVAL'): continue
    env[k]=v.replace('{OUT}',out+'/ref')
log=open(out+'/ref.log','w')
p=subprocess.Popen([B+'/build/PS2Recomp/ps2xRuntime/ps2EntryRunner',elf],env=env,stdout=log,stderr=subprocess.STDOUT)
time.sleep(1.0)
elfb=open(elf,'rb').read()
ph=struct.unpack_from('<I',elfb,28)[0]
off,va=[(o,v) for t,o,v,*_ in [struct.unpack_from('<6I',elfb,ph+i*32) for i in range(struct.unpack_from('<H',elfb,44)[0])] if t==1][0]
sig=elfb[off+0x100008-va:off+0x100008-va+64]
mem=open(f'/proc/{p.pid}/mem','rb',0)
base=None
while base is None and p.poll() is None:
    for l in open(f'/proc/{p.pid}/maps'):
        m=re.match(r'([0-9a-f]+)-([0-9a-f]+) rw',l)
        if not m: continue
        a,b=int(m[1],16),int(m[2],16)
        if b-a<0x2000000: continue
        try:
            mem.seek(a); pos=a
            while pos<b and base is None:
                chunk=mem.read(min(0x4000000,b-pos))
                k=chunk.find(sig)
                if k>=0: base=pos+k-0x100008
                pos+=len(chunk)
        except OSError as e: pass
        if base is not None: break
    time.sleep(0.2)
print('rdram at',hex(base or 0),flush=True)
def word(a):
    mem.seek(base+a); return struct.unpack('<I',mem.read(4))[0]
i=0;t0=time.time();last=-1
while p.poll() is None and i<len(marks) and time.time()-t0<float(os.environ.get('REF_SECS','1500')):
    v=word(0x46e608)
    if v!=last and v%300==0: print(f'{time.time()-t0:.1f}s counter {v}',flush=True)
    last=v
    if v>=marks[i]:
        os.kill(p.pid,signal.SIGSTOP)
        v=word(0x46e608)
        mem.seek(base); d=mem.read(0x2000000)
        open(f'{out}/ram_{marks[i]}.bin','wb').write(d)
        print(f'dump at counter {v} for {marks[i]}',flush=True)
        os.kill(p.pid,signal.SIGCONT)
        i+=1
    time.sleep(0.002)
if p.poll() is None: p.kill()
print('end',p.wait(),flush=True)
