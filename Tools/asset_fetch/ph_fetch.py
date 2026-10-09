"""Poly Haven fetcher (CC0). Sequential, polite, md5-verified.
usage: ph_fetch.py <raw_assets dir> <kind: tex|model|hdri> <id>:<res> [<id>:<res> ...]
Each asset goes to <raw_assets>/polyhaven/<id>/ (must not exist). Appends a JSON record to <raw_assets>/_cache/records.jsonl
Textures: Diffuse(jpg) nor_dx(png) Rough(jpg) AO(jpg) Displacement(png) [+Metal(jpg) if present]
Models: glTF (jpg textures, ARM-packed) at given res. HDRI: .hdr
"""
import sys,os,json,hashlib,time,urllib.request
UA="driving-game-asset-fetch/1.0 (personal UE project)"
raw,kind=sys.argv[1],sys.argv[2]
def get(u):
    return urllib.request.urlopen(urllib.request.Request(u,headers={"User-Agent":UA}),timeout=120)
def dl(url,dest,md5=None):
    os.makedirs(os.path.dirname(dest),exist_ok=True)
    for attempt in range(3):
        try:
            h=hashlib.md5()
            with get(url) as r, open(dest,'wb') as f:
                while True:
                    b=r.read(1<<20)
                    if not b: break
                    f.write(b);h.update(b)
            if md5 and h.hexdigest()!=md5: raise IOError('md5 mismatch')
            time.sleep(0.4);return os.path.getsize(dest)
        except Exception as e:
            print('retry',url,e);time.sleep(3)
    raise SystemExit('failed '+url)
def info(i):
    return json.load(get(f"https://api.polyhaven.com/info/{i}"))
for spec in sys.argv[3:]:
    i,res=spec.split(':')
    d=os.path.join(raw,'polyhaven',i)
    if os.path.exists(d): print('skip exists',i);continue
    os.makedirs(d)
    files=json.load(get(f"https://api.polyhaven.com/files/{i}")); inf=info(i)
    rec=dict(id=i,kind=kind,source='polyhaven',path=d,res=res,url=f"https://polyhaven.com/a/{i}",license='CC0',
             authors=list(inf.get('authors',{}).keys()),categories=inf.get('categories',[]),maps=[],missing=[],bytes=0)
    if kind=='tex':
        for key,fmt,tag in [('Diffuse','jpg','diff'),('nor_dx','png','nor_dx'),('Rough','jpg','rough'),('AO','jpg','ao'),('Displacement','png','disp'),('Metal','jpg','metal')]:
            e=files.get(key,{}).get(res,{}).get(fmt)
            if not e:
                alt=files.get(key,{}).get(res,{})
                if key not in('Metal',): rec['missing'].append(tag)
                continue
            rec['bytes']+=dl(e['url'],os.path.join(d,os.path.basename(e['url'])),e['md5']);rec['maps'].append(tag)
        rec['normal']='DirectX (nor_dx)'
    elif kind=='model':
        e=files['gltf'][res]['gltf']
        rec['bytes']+=dl(e['url'],os.path.join(d,os.path.basename(e['url'])))
        for rel,v in e['include'].items(): rec['bytes']+=dl(v['url'],os.path.join(d,rel),v['md5'])
        rec['maps']=['gltf+jpg textures (diff, nor_gl, arm)']
    elif kind=='hdri':
        e=files['hdri'][res]['hdr']
        rec['bytes']+=dl(e['url'],os.path.join(d,os.path.basename(e['url'])),e['md5']);rec['maps']=['hdr']
    open(os.path.join(raw,'_cache','records.jsonl'),'a').write(json.dumps(rec)+'\n')
    print(kind,i,res,rec['bytes']//1024,'KB','missing:',rec['missing'])
