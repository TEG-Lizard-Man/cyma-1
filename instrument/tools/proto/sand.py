# SPDX-License-Identifier: MPL-2.0
# CYMA-1 prototype (reference only, not used by the build): copied verbatim from the design-phase scratchpad.
# tools/gen_plates.py is the production generator; this file documents where its constants came from.
import numpy as np
from PIL import Image
G=24; P=184; rng=np.random.default_rng(1)
def down(F):  # 64->24 area-ish resample via bilinear sampling
    N=F.shape[-1]; c=(np.arange(G)+0.5)/G*N-0.5
    i0=np.clip(np.floor(c).astype(int),0,N-2); f=c-i0
    A=F[...,i0,:]*(1-f)[:,None]+F[...,i0+1,:]*f[:,None]
    B=A[...,:,i0]*(1-f)+A[...,:,i0+1]*f
    return B
def bil(Fg,x,y):  # x,y in [0,1)
    gx=x*G-0.5; gy=y*G-0.5; i=np.clip(np.floor(gx).astype(int),0,G-2); j=np.clip(np.floor(gy).astype(int),0,G-2)
    fx=gx-i; fy=gy-j
    return (Fg[j,i]*(1-fx)*(1-fy)+Fg[j,i+1]*fx*(1-fy)+Fg[j+1,i]*(1-fx)*fy+Fg[j+1,i+1]*fx*fy)
def run(nm,strike=(0.62,0.40),nmodes=16,hard=0.6,steps=240,out=None,p=1.0):
    d=np.load(nm+'.npz'); m=d['m']; F=d['F'][:nmodes]; r=d['ratios'][:nmodes]**p
    F=F/np.abs(F).max(axis=(1,2),keepdims=True)
    Fg=down(F); Fg=np.round(Fg*127)/127   # int8 quantisation as on device
    mg=down(m.astype(float))
    # strike weights (coherent projection per degenerate group)
    sx,sy=strike; w=np.array([bil(Fg[k],np.array([sx]),np.array([sy]))[0] for k in range(nmodes)])
    groups=[]; k=0
    while k<nmodes:
        g=[k]
        while k+1<nmodes and abs(r[k+1]-r[k])/r[k]<2e-3: k+=1; g.append(k)
        groups.append(g); k+=1
    Psi=[]; eg=[]
    tau=0.0008+0.004*(1-hard); f0=220.0
    for g in groups:
        ps=sum(w[i]*Fg[i] for i in g); n=np.abs(ps).max()+1e-9; Psi.append(ps/n)
        f=f0*np.sqrt(r[g[0]])  # membrane freq ratio is sqrt(lambda)
        spec=abs(np.cos(np.pi*f*tau)/(1-(2*f*tau)**2+1e-6))
        amp=n*spec; acc=amp*(f/f0)**2      # acceleration weighting
        eg.append(acc**2)
    eg=np.array(eg); eg/=eg.max()
    E=sum(e*ps**2 for e,ps in zip(eg,Psi))
    E=E/E.max()
    # sand
    n=1600; pts=np.zeros((0,2))
    while len(pts)<n:
        c=rng.random((n,2)); inside=bil(mg,c[:,0],c[:,1])>0.5; pts=np.vstack([pts,c[inside]])
    pts=pts[:n]
    for s in range(steps):
        a=np.sqrt(np.clip(bil(E,pts[:,0],pts[:,1]),0,None))
        lift=np.clip(a-0.06,0,None)
        step=0.020*lift[:,None]*(rng.random((n,2))+rng.random((n,2))-1)*1.7
        e=1/G; gx=(bil(E,pts[:,0]+e,pts[:,1])-bil(E,pts[:,0]-e,pts[:,1])); gy=(bil(E,pts[:,0],pts[:,1]+e)-bil(E,pts[:,0],pts[:,1]-e))
        new=pts+step-0.004*np.stack([gx,gy],1)*(lift>0)[:,None]
        new=np.clip(new,0.001,0.999); ok=bil(mg,new[:,0],new[:,1])>0.5
        pts[ok]=new[ok]
    # render: plate shading + density splat
    img=np.zeros((P,P,3)); yy,xx=np.mgrid[0:P,0:P]; u=(xx+0.5)/P; v=(yy+0.5)/P
    inside=bil(mg,u.ravel(),v.ravel()).reshape(P,P)>0.5
    Ef=bil(E,u.ravel(),v.ravel()).reshape(P,P)
    base=np.array([0x12,0x18,0x26])/255; tint=np.array([0x1c,0x34,0x52])/255
    img[inside]=base+(tint-base)*np.sqrt(np.clip(Ef[inside],0,1))[:,None]*0.8
    dens=np.zeros((P,P))
    X=pts[:,0]*P-0.5; Y=pts[:,1]*P-0.5; i=np.floor(X).astype(int); j=np.floor(Y).astype(int); fx=X-i; fy=Y-j
    for di,dj,wt in ((0,0,(1-fx)*(1-fy)),(1,0,fx*(1-fy)),(0,1,(1-fx)*fy),(1,1,fx*fy)):
        np.add.at(dens,(np.clip(j+dj,0,P-1),np.clip(i+di,0,P-1)),wt)
    from scipy.ndimage import uniform_filter
    glow=uniform_filter(dens,3)
    t=1-np.exp(-(dens*0.9+glow*0.6))
    sand=np.array([0xF4,0xE6,0xC8])/255; gold=np.array([0xC9,0xA8,0x6A])/255
    col=gold[None,None,:]*(1-t[...,None])+sand*t[...,None]
    img=img*(1-t[...,None])+col*t[...,None]
    Image.fromarray((np.clip(img,0,1)*255).astype(np.uint8)).resize((P*2,P*2),Image.NEAREST).save(out)
for nm,st,p in [('circle',(0.70,0.42),1.0),('hex',(0.60,0.35),1.0),('star',(0.52,0.30),1.0),('guitar',(0.55,0.62),1.0),('square',(0.30,0.22),1.0)]:
    run(nm,strike=st,out=nm+'_sand.png',p=p)
print('ok')
