import struct,sys,math
src,dst=sys.argv[1],sys.argv[2]
d=open(src,'rb').read()
bs,fpm,nb,dirsz,_,bma=struct.unpack('<6I',d[32:56])
ndb=math.ceil(dirsz/bs)
nmap=math.ceil(ndb*4/bs)
mapbytes=b''.join(d[(bma+i)*bs:(bma+i+1)*bs] for i in range(nmap))
dblocks=struct.unpack('<%dI'%ndb,mapbytes[:ndb*4])
dirb=b''.join(d[b*bs:(b+1)*bs] for b in dblocks)[:dirsz]
ns=struct.unpack('<I',dirb[:4])[0]
sizes=list(struct.unpack('<%dI'%ns,dirb[4:4+4*ns]))
off=4+4*ns
streams=[]
for s in sizes:
    if s==0xFFFFFFFF: streams.append(None); continue
    n=math.ceil(s/bs); bl=struct.unpack('<%dI'%n,dirb[off:off+4*n]); off+=4*n
    streams.append(b''.join(d[b*bs:(b+1)*bs] for b in bl)[:s])
NB=4096
out=[None]*3  # block0 super, 1,2 fpm
blocks=[]
def alloc(data):
    res=[]
    for i in range(0,len(data),NB):
        # skip fpm positions (block k*NB+1, +2)
        while (len(out)%NB) in (1,2): out.append(b'\xff'*NB)
        res.append(len(out)); out.append(data[i:i+NB].ljust(NB,b'\0'))
    return res
nd=b''
lists=[]
for s in streams:
    if s is None: lists.append([]); continue
    lists.append(alloc(s))
newdir=struct.pack('<I',ns)+b''.join(struct.pack('<I',0xFFFFFFFF if s is None else len(s)) for s in streams)+b''.join(struct.pack('<%dI'%len(l),*l) for l in lists)
dl=alloc(newdir)
assert len(dl)*4<=NB
ml=alloc(struct.pack('<%dI'%len(dl),*dl))
while (len(out)%NB) in (1,2): out.append(b'\xff'*NB)
total=len(out)
sup=d[:32]+struct.pack('<6I',NB,1,total,len(newdir),0,ml[0])
out[0]=sup.ljust(NB,b'\0'); out[1]=b'\xff'*NB; out[2]=b'\xff'*NB
with open(dst,'wb') as f:
    for b in out: f.write(b if b else b'\xff'*NB)
print('streams',ns,'blocks',total)
