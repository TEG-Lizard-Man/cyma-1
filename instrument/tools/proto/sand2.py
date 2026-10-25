# SPDX-License-Identifier: MPL-2.0
# CYMA-1 prototype (reference only, not used by the build): copied verbatim from the design-phase scratchpad.
# tools/gen_plates.py is the production generator; this file documents where its constants came from.
import numpy as np
from PIL import Image
from scipy.ndimage import uniform_filter
exec(open('sand.py').read().split('def run')[0])
def chladni_square(K=16):
    N=64; y,x=np.mgrid[0:N,0:N]; u=(x+0.5)/N; v=(y+0.5)/N
    pairs=[(m,n) for m in range(0,7) for n in range(m,7) if (m,n)!=(0,0) and (m,n)!=(0,1)]
    F=[];lam=[]
    for m,n in pairs:
        for s in ([1,-1] if m!=n else [1]):
            f=np.cos(m*np.pi*u)*np.cos(n*np.pi*v)+s*np.cos(n*np.pi*u)*np.cos(m*np.pi*v)
            if np.abs(f).max()<1e-6: continue
            F.append(f); lam.append((m*m+n*n)+0.3*s*(m!=n)*0.5)
    o=np.argsort(lam)[:K]; F=np.array(F)[o]; lam=np.array(lam)[o]
    return np.ones((N,N),bool),lam/lam[0],F
def sim(m,F,eg_groups,steps=300,n=1800,seed=2):
    rng=np.random.default_rng(seed)
    mg=down(m.astype(float)); 
    E=sum(e*ps**2 for e,ps in eg_groups); E/=E.max()
    pts=np.zeros((0,2))
    while len(pts)<n:
        c=rng.random((n,2)); pts=np.vstack([pts,c[bil(mg,c[:,0],c[:,1])>0.55]])
    pts=pts[:n]; e=1/G
    for s in range(steps):
        a=np.sqrt(np.clip(bil(E,pts[:,0],pts[:,1]),0,None)); lift=np.clip(a-0.05,0,None)
        step=0.03*lift[:,None]*(rng.random((n,2))+rng.random((n,2))-1)
        gx=bil(E,pts[:,0]+e,pts[:,1])-bil(E,pts[:,0]-e,pts[:,1]); gy=bil(E,pts[:,0],pts[:,1]+e)-bil(E,pts[:,0],pts[:,1]-e)
        new=np.clip(pts+step-0.006*np.stack([gx,gy],1)*(lift>0)[:,None],0.002,0.998)
        ok=bil(mg,new[:,0],new[:,1])>0.55; pts[ok]=new[ok]
    P=184; img=np.zeros((P,P,3)); yy,xx=np.mgrid[0:P,0:P]; u=(xx+0.5)/P; v=(yy+0.5)/P
    inside=bil(mg,u.ravel(),v.ravel()).reshape(P,P)>0.55
    Ef=bil(E,u.ravel(),v.ravel()).reshape(P,P)
    base=np.array([0x12,0x18,0x26])/255; tint=np.array([0x1e,0x3a,0x5c])/255
    img[:]=np.array([0x0A,0x0B,0x10])/255
    img[inside]=base+(tint-base)*np.sqrt(np.clip(Ef[inside],0,1))[:,None]*0.9
    dens=np.zeros((P,P)); X=pts[:,0]*P-0.5; Y=pts[:,1]*P-0.5; i=np.floor(X).astype(int); j=np.floor(Y).astype(int); fx=X-i; fy=Y-j
    for di,dj,wt in ((0,0,(1-fx)*(1-fy)),(1,0,fx*(1-fy)),(0,1,(1-fx)*fy),(1,1,fx*fy)):
        np.add.at(dens,(np.clip(j+dj,0,P-1),np.clip(i+di,0,P-1)),wt)
    glow=uniform_filter(dens,3); t=1-np.exp(-(dens*0.8+glow*0.7))
    sand=np.array([0xFF,0xF2,0xD8])/255; gold=np.array([0xC9,0xA8,0x6A])/255
    col=gold*(1-t[...,None])+sand*t[...,None]; img=img*(1-t[...,None])+col*t[...,None]
    return Image.fromarray((np.clip(img,0,1)*255).astype(np.uint8)).resize((P*2,P*2),Image.NEAREST)
def groups_from(F,r,strike,weights):
    Fg=down(F/np.abs(F).max(axis=(1,2),keepdims=True)); Fg=np.round(Fg*127)/127
    w=[bil(Fg[k],np.array([strike[0]]),np.array([strike[1]]))[0] for k in range(len(F))]
    out=[];k=0
    while k<len(F):
        g=[k]
        while k+1<len(F) and abs(r[k+1]-r[k])/r[k]<2e-3: k+=1; g.append(k)
        ps=sum(w[i]*Fg[i] for i in g); nrm=np.abs(ps).max()+1e-9
        out.append((weights[g[0]]*nrm**2, ps/nrm)); k+=1
    return out
ims=[]
for nm,mode_sel,st in [('circle',[9,10],(0.7,0.4)),('hex',[13,14],(0.6,0.35)),('guitar',[11],(0.5,0.6)),('star',[12,13],(0.55,0.3))]:
    d=np.load(nm+'.npz'); F=d['F'][:16]; r=d['ratios'][:16]
    wts=np.full(16,0.02); wts[mode_sel]=1.0  # a bowed mode dominating, others faint
    ims.append(sim(d['m'],F,groups_from(F,r,st,wts)))
m,r,F=chladni_square(); wts=np.full(16,0.02); wts[[7]]=1.0
ims.append(sim(m,F,groups_from(F,r,(0.3,0.2),wts)))
# struck mixed: circle, hard mallet, modes weighted by accel
d=np.load('circle.npz'); F=d['F'][:16]; r=d['ratios'][:16]; f=np.sqrt(r)
wts=(f**2*np.abs(np.sinc(f*0.25)))**2; wts/=wts.max()
ims.append(sim(d['m'],F,groups_from(F,r,(0.72,0.45),wts)))
W=sum(i.width for i in ims); o=Image.new('RGB',(W,ims[0].height),(10,11,16)); x=0
for i in ims: o.paste(i,(x,0)); x+=i.width
o.save('strip2.png'); print('ok')
