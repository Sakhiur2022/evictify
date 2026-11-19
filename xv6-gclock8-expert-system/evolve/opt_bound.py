import sys, os, math, heapq
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE); sys.path.insert(0,os.path.join(HERE,'..','bcsim'))
from refmodel import gen
import suite
def trace(cfg,nbuf):
    w,scan,hot,interval,passes,rounds=cfg
    return [b for b,h in gen(w,nbuf,scan,hot,interval,passes,rounds) if b!="PROBE"]
def lru(tr,n):
    from collections import OrderedDict
    c=OrderedDict();m=0
    for b in tr:
        if b in c: c.move_to_end(b)
        else:
            m+=1
            if len(c)>=n: c.popitem(last=False)
            c[b]=1
    return m
def opt(tr,n):
    N=len(tr); nxt=[0]*N; last={}
    for i in range(N-1,-1,-1):
        nxt[i]=last.get(tr[i],10**9); last[tr[i]]=i
    cache={};h=[];m=0
    for i,b in enumerate(tr):
        if b in cache: cache[b]=nxt[i]; heapq.heappush(h,(-nxt[i],b)); continue
        m+=1
        if len(cache)>=n:
            while True:
                d,v=heapq.heappop(h)
                if v in cache and cache[v]==-d: del cache[v]; break
        cache[b]=nxt[i]; heapq.heappush(h,(-nxt[i],b))
    return m
def main():
    for name,cfgs,nb in (("train",suite.TRAIN,suite.TRAIN_NBUF),("held",suite.HELDOUT,suite.HELDOUT_NBUF)):
        rs=[]
        for nm,cfg in cfgs:
            for n in nb:
                t=trace(cfg,n); l=lru(t,n); o=opt(t,n)
                rs.append(math.log((l*4000+ (len(t)-l)*10)/(o*4000+(len(t)-o)*10)))
                if name=="train" and n==60: print(nm,n,len(t),"LRU",l,"OPT",o)
        print(name,"OPT/LRU speedup GM ceiling:",math.exp(sum(rs)/len(rs)))

if __name__=='__main__':
    main()
