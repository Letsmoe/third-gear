"""Add Zusatzzeichen (supplementary signs) SVGs from Commons and merge into _cache/signs.json. usage: zz_fetch.py <raw_assets dir>"""
import sys,os,re,json,time,subprocess,urllib.parse
raw=sys.argv[1];UA="driving-game-asset-fetch/1.0 (personal UE project; polite sequential requests)"
T={'1001-30':'Zusatzzeichen 1001-30 - auf ... m, StVO 1992.svg','1000-10':'Zusatzzeichen 1000-10 - Richtungsangaben durch Pfeile, linksweisend, StVO 1992.svg',
 '1000-20':'Zusatzzeichen 1000-20 - Richtungsangaben durch Pfeile, rechtsweisend, StVO 1992.svg','1020-30':'Zusatzzeichen 1020-30 - Anlieger frei (600x330), StVO 1992.svg',
 '1022-10':'Zusatzzeichen 1022-10 - Radfahrer frei, StVO 1992.svg','1040-30':'Zusatzzeichen 1040-30 - Zeitliche Beschränkung (16 - 18 h), 330x600, StVO 1992.svg',
 '1052-30':'Zusatzzeichen 1052-30 - Streckenverbot für den Transport gefährlicher Güter auf Straßen, StVO 1992.svg'}
def curl(args):
    for a in range(6):
        r=subprocess.run(['/usr/bin/curl','-sSfL','--max-time','60','-A',UA]+args,capture_output=True)
        if r.returncode==0: return r.stdout
        print('retry',r.stderr[:80]);time.sleep(40*(a+1))
    raise SystemExit('fail')
titles='|'.join('File:'+t for t in T.values())
r=json.loads(curl(['-G','https://commons.wikimedia.org/w/api.php','--data-urlencode','action=query','--data-urlencode','format=json','--data-urlencode','prop=imageinfo','--data-urlencode','iiprop=url|extmetadata','--data-urlencode','titles='+titles]))
meta={p['title']:p['imageinfo'][0] for p in r['query']['pages'].values() if 'imageinfo' in p}
sj=os.path.join(raw,'_cache','signs.json');s=json.load(open(sj))
s['recs']=[x for x in s['recs'] if x['code']!='1052-30']
bad=os.path.join(raw,'signs','svg','Zeichen_1052-30.svg')
if os.path.exists(bad): os.remove(bad)
s['notfound']=[c for c in s['notfound'] if c not in T]
for c,t in T.items():
    pg=meta['File:'+t];url=pg['url'].split('?')[0];fn=f'Zusatzzeichen_{c}.svg';dest=os.path.join(raw,'signs','svg',fn)
    if not os.path.exists(dest):
        d=curl([url]);assert b'<svg' in d[:3000];open(dest,'wb').write(d);time.sleep(4)
    md=pg['extmetadata']
    s['recs'].append(dict(code='ZZ '+c,file=fn,title='File:'+t,url=url,page='File:'+urllib.parse.quote(t.replace(' ','_')),license=md.get('LicenseShortName',{}).get('value'),usage='',artist='',alternatives=[]))
    print(c,s['recs'][-1]['license'])
json.dump(s,open(sj,'w'),indent=1)
