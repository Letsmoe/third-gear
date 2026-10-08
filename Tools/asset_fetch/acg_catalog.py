import json,sys,time,urllib.request
out=sys.argv[1]; TYPE=sys.argv[2] if len(sys.argv)>2 else "Material"
UA="driving-game-asset-fetch/1.0 (personal UE project)"
res=[];off=0
while True:
    u=f"https://ambientcg.com/api/v2/full_json?type={TYPE}&limit=100&offset={off}&include=tagData,downloadData&sort=Alphabet"
    r=json.load(urllib.request.urlopen(urllib.request.Request(u,headers={"User-Agent":UA}),timeout=60))
    for a in r['foundAssets']:
        dl=[]
        try: cats=a['downloadFolders']['default']['downloadFiletypeCategories']
        except Exception: cats={}
        if not isinstance(cats,dict): cats={}
        for f in cats.get('zip',{}).get('downloads',[]):
            dl.append((f['attribute'],f['size'],f['fullDownloadPath']))
        res.append(dict(id=a['assetId'],method=a['creationMethod'],cat=a['displayCategory'],tags=a['tags'],dl=dl))
    off+=100
    if off>=r['numberOfResults']: break
    time.sleep(1)
json.dump(res,open(out,'w'))
print(len(res))
