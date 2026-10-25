# SPDX-License-Identifier: MPL-2.0
# CYMA-1 prototype (reference only, not used by the build): copied verbatim from the design-phase scratchpad.
# tools/gen_plates.py is the production generator; this file documents where its constants came from.
import numpy as np, time, scipy.sparse as sp, scipy.sparse.linalg as sla
from PIL import Image
N=64
def mask_shape(name):
    y,x=np.mgrid[0:N,0:N]; u=(x+0.5)/N*2-1; v=(y+0.5)/N*2-1
    r=np.hypot(u,v); th=np.arctan2(v,u)
    if name=='circle': m=r<0.96
    elif name=='hex':
        m=np.ones_like(r,bool)
        for k in range(6):
            a=k*np.pi/3; m&=(u*np.cos(a)+v*np.sin(a))<0.86
    elif name=='star':
        rr=0.62+0.30*np.cos(5*th); m=r<rr
    elif name=='guitar':
        m=(np.hypot(u,(v-0.38)/1.0)<0.58)|(np.hypot(u,(v+0.30))<0.44)|((abs(u)<0.36)&(abs(v-0.05)<0.3))
    elif name=='square': m=(abs(u)<0.9)&(abs(v)<0.9)&~((abs(u-0.31)<0.0)&(abs(v)<0))
    return m
def solve(name,K=32):
    m=mask_shape(name); idx=-np.ones((N,N),int); pts=np.argwhere(m); idx[m]=np.arange(len(pts))
    rows=[];cols=[];vals=[]
    for i,(a,b) in enumerate(pts):
        rows.append(i);cols.append(i);vals.append(4.0)
        for da,db in ((1,0),(-1,0),(0,1),(0,-1)):
            j=idx[a+da,b+db] if 0<=a+da<N and 0<=b+db<N else -1
            if j>=0: rows.append(i);cols.append(j);vals.append(-1.0)
    L=sp.csr_matrix((vals,(rows,cols)),shape=(len(pts),len(pts)))
    t=time.time(); w,V=sla.eigsh(L,k=K,sigma=0,which='LM'); dt=time.time()-t
    o=np.argsort(w); w=w[o]; V=V[:,o]
    F=np.zeros((K,N,N)); 
    for k in range(K): F[k][m]=V[:,k]
    return m,w/w[0],F,dt
for nm in ['circle','hex','star','guitar','square']:
    m,ratios,F,dt=solve(nm)
    print(nm,'%.2fs'%dt,'membrane sqrt ratios',np.round(np.sqrt(ratios[:10]),3))
    np.savez(nm+'.npz',m=m,ratios=ratios,F=F)
