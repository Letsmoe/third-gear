"""ambientCG fetcher (CC0). usage: acg_fetch.py <raw_assets dir> <catalog acg.json> <AssetId>:<attr e.g. 4K-PNG> ...
Downloads the zip to a temp name OUTSIDE the asset dir, extracts only image maps (no NormalGL, no scripts/other) into a fresh
<raw_assets>/ambientcg/<AssetId>/, using safe basenames only. Appends record to <raw_assets>/_cache/records.jsonl"""
import sys,os,json,zipfile,time,urllib.request,re
raw,cat=sys.argv[1],json.load(open(sys.argv[2]))
UA="driving-game-asset-fetch/1.0 (personal UE project)"
bi={a['id']:a for a in cat}
tmpdir=os.path.join(raw,'_cache','zips');os.makedirs(tmpdir,exist_ok=True)
for spec in sys.argv[3:]:
    i,attr=spec.split(':');a=bi[i]
    d=os.path.join(raw,'ambientcg',i)
    if os.path.exists(d): print('skip',i);continue
    url=[u for at,s,u in a['dl'] if at==attr][0]
    z=os.path.join(tmpdir,f'{i}_{attr}.zip')
    for t in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url,headers={"User-Agent":UA}),timeout=300) as r, open(z,'wb') as f:
                while True:
                    b=r.read(1<<20)
                    if not b:break
                    f.write(b)
            break
        except Exception as e: print('retry',e);time.sleep(5)
    os.makedirs(d)
    maps=[]
    with zipfile.ZipFile(z) as zf:
        for n in zf.namelist():
            b=os.path.basename(n)
            if not re.search(r'\.(png|jpg)$',b,re.I) or '_NormalGL' in b or b.startswith('.') or '/' in b.replace('\\','/').strip('/') and False: continue
            with zf.open(n) as src, open(os.path.join(d,b),'wb') as dst: dst.write(src.read())
            m=re.search(r'_([A-Za-z]+)\.(png|jpg)$',b);maps.append(m.group(1) if m else b)
    os.remove(z)
    size=sum(os.path.getsize(os.path.join(d,x)) for x in os.listdir(d))
    miss=[m for m in ('Color','NormalDX','Roughness','Displacement','AmbientOcclusion') if m not in maps]
    rec=dict(id=i,kind='tex',source='ambientcg',path=d,res=attr,url=f"https://ambientcg.com/a/{i}",license='CC0',method=a['method'],maps=maps,missing=miss,bytes=size,categories=[a['cat']],normal='DirectX (NormalDX)')
    open(os.path.join(raw,'_cache','records.jsonl'),'a').write(json.dumps(rec)+'\n')
    print(i,attr,size//1024,'KB maps',maps,'missing',miss);time.sleep(2)
