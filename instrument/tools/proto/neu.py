# SPDX-License-Identifier: MPL-2.0
# CYMA-1 prototype (reference only, not used by the build): copied verbatim from the design-phase scratchpad.
# tools/gen_plates.py is the production generator; this file documents where its constants came from.
import numpy as np, scipy.sparse as sp, scipy.sparse.linalg as sla
from PIL import Image
import sand2 as S0
exec(open('modes.py').read().split('def solve')[0])
def solve_neu(name,K=34):
    m=mask_shape(name); idx=-np.ones((N,N),int); pts=np.argwhere(m); idx[m]=np.arange(len(pts))
    rows=[];cols=[];vals=[]
    for i,(a,b) in enumerate(pts):
        nb=0
        for da,db in ((1,0),(-1,0),(0,1),(0,-1)):
            j=idx[a+da,b+db] if 0<=a+da<N and 0<=b+db<N else -1
            if j>=0: rows.append(i);cols.append(j);vals.append(-1.0); nb+=1
        rows.append(i);cols.append(i);vals.append(float(nb))
    L=sp.csr_matrix((vals,(rows,cols)),shape=(len(pts),len(pts)))
    w,V=sla.eigsh(L+sp.identity(len(pts))*1e-3,k=K,sigma=0,which='LM'); o=np.argsort(w); w=w[o];V=V[:,o]
    w=w[1:];V=V[:,1:]   # drop the constant mode
    F=np.zeros((len(w),N,N))
    for k in range(len(w)): F[k][m]=V[:,k]
    return m,w/w[0],F
ims=[]
for G in (24,32):
    S0.G=G
    for nm,sel,st in [('circle',[8,9],(0.7,0.4)),('hex',[10,11],(0.6,0.35)),('guitar',[9],(0.5,0.6)),('star',[10,11],(0.55,0.3))]:
        m,r,F=solve_neu(nm); F=F[:16]; r=r[:16]
        print(nm,'neumann ratios',np.round(r[:8],3))
        wts=np.full(16,0.03); wts[sel]=1.0
        ims.append(S0.sim(m,F,S0.groups_from(F,r,st,wts)))
    # struck mix circle
    m,r,F=solve_neu('circle'); F=F[:16]; r=r[:16]; f=r
    wts=(f**2*np.abs(np.sinc(f*0.12)))**2; wts/=wts.max()
    ims.append(S0.sim(m,F,S0.groups_from(F,r,(0.72,0.45),wts)))
W=5*ims[0].width; H=2*ims[0].height; o=Image.new('RGB',(W,H),(10,11,16))
for k,i in enumerate(ims): o.paste(i,((k%5)*i.width,(k//5)*i.height))
o.save('strip3.png'); print('ok')
