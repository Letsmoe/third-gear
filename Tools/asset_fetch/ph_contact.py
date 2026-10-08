# usage: ph_contact.py <outdir> <outname> id1 id2 ...   (downloads Poly Haven thumbnails, builds labelled contact sheet via ImageMagick montage)
import sys,os,subprocess,urllib.request,time
outdir,name=sys.argv[1],sys.argv[2];ids=sys.argv[3:]
os.makedirs(outdir,exist_ok=True)
UA="driving-game-asset-fetch/1.0 (personal UE project)"
files=[]
for i in ids:
    p=os.path.join(outdir,i+'.png')
    if not os.path.exists(p):
        try:
            u=f"https://cdn.polyhaven.com/asset_img/thumbs/{i}.png?width=256&height=256"
            open(p,'wb').write(urllib.request.urlopen(urllib.request.Request(u,headers={"User-Agent":UA}),timeout=30).read())
        except Exception as e: print('fail',i,e); continue
        time.sleep(0.2)
    files.append(p)
subprocess.run(['montage','-label','%t','-geometry','256x256+4+4','-tile','6x','-pointsize','14']+files+[os.path.join(outdir,name)],check=True)
