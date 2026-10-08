"""Fetch German StVO sign SVGs from Wikimedia Commons. usage: signs_fetch.py <RawAssets dir>
Looks up 'File:Zeichen <code> ...svg' via allpages prefix, picks newest StVO year, records license from extmetadata."""
import sys,os,re,json,time,subprocess,urllib.request,urllib.parse
raw=sys.argv[1]
UA="driving-game-asset-fetch/1.0 (personal UE project; polite sequential requests)"
API="https://commons.wikimedia.org/w/api.php"
def api(**p):
    p['format']='json'
    u=API+'?'+urllib.parse.urlencode(p)
    for a in range(6):
        r=subprocess.run(['/usr/bin/curl','-sSf','--max-time','60','-A',UA,u],capture_output=True)
        if r.returncode==0:
            time.sleep(1);return json.loads(r.stdout)
        print('retry api',r.stderr[:80]);time.sleep(20*(a+1))
    raise SystemExit('api fail')
speeds=[10,20,30,40,50,60,70,80,100,120]
codes=("101 102 103-10 103-20 108 108-10 110-10 120 123 131 133 136 138 205 206 208 209 211 214 220 222 237 239 240 241 250 267 274 274.1 274.2 276 278 282 283 286 301 306 307 310 311 325.1 325.2 350 357 1001-30 "
       "274-"+" 274-".join(map(str,speeds))+" 278-"+" 278-".join(map(str,[30,50,60,80]))+" 274.1-20 274.2-20 274.2-30 274.2-50 282-50 1000-10 1000-20 1010-51 1020-30 1022-10 1040-30 1052-30").split()
out=os.path.join(raw,'signs','svg'); os.makedirs(out,exist_ok=True)
# 1) list every 'File:Zeichen ...' svg once (few big calls instead of one call per sign)
allsvg=[];cont={}
while True:
    r=api(action='query',list='allpages',apnamespace=6,apprefix='Zeichen ',aplimit=500,**cont)
    allsvg+=[p['title'] for p in r['query']['allpages'] if p['title'].lower().endswith('.svg')]
    if 'continue' in r: cont={'apcontinue':r['continue']['apcontinue']}
    else: break
print('svg files with prefix Zeichen:',len(allsvg))
def year(n):
    m=re.search(r'StVO (\d{4})',n);return int(m.group(1)) if m else 0
chosen={};notfound=[];alts={}
for c in codes:
    pat=re.compile(r'^File:Zeichen '+re.escape(c)+r' - ')
    names=[n for n in allsvg if pat.match(n)]
    good=[n for n in names if not re.search(r'Leuchttafel|Zusatz|Kombination|mit |Auflösung',n.split(' - ',1)[-1])] or names
    if not good:
        names=[n for n in allsvg if n.startswith(f'File:Zeichen {c} ')]
        good=names
    if not good: notfound.append(c);continue
    chosen[c]=sorted(good,key=year)[-1];alts[c]=names
# 2) batched imageinfo
meta={}
titles=sorted(set(chosen.values()))
for i in range(0,len(titles),40):
    r=api(action='query',titles='|'.join(titles[i:i+40]),prop='imageinfo',iiprop='url|extmetadata')
    for p in r['query']['pages'].values():
        if 'imageinfo' in p: meta[p['title']]=p['imageinfo'][0]
recs=[]
for c,n in chosen.items():
    pg=meta[n];md=pg['extmetadata'];url=pg['url'].split('?')[0]
    fn=f"Zeichen_{c}.svg";dest=os.path.join(out,fn)
    if not (os.path.exists(dest) and os.path.getsize(dest)>100):
        for a in range(8):
            r=subprocess.run(['/usr/bin/curl','-sSfL','--max-time','60','-A',UA,url],capture_output=True)
            if r.returncode==0 and b'<svg' in r.stdout[:3000]:
                data=r.stdout;break
            print('retry dl',r.stderr[:100]);time.sleep(60*(a+1))
        else: raise SystemExit('download failed '+c)
        open(dest,'wb').write(data);time.sleep(4)
    recs.append(dict(code=c,file=fn,title=n,url=url,page='https://commons.wikimedia.org/wiki/'+urllib.parse.quote(n[5:].replace(' ','_')).join(['File:','']),
        license=md.get('LicenseShortName',{}).get('value'),usage=md.get('UsageTerms',{}).get('value'),artist=re.sub('<[^>]+>','',md.get('Artist',{}).get('value',''))[:80],alternatives=alts[c]))
    print(c,'->',n,'|',recs[-1]['license'],flush=True)
json.dump(dict(recs=recs,notfound=notfound),open(os.path.join(raw,'_cache','signs.json'),'w'),indent=1)
print('notfound',notfound)
