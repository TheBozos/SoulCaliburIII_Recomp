import subprocess,sys
def raw(p): return subprocess.check_output(['magick',p,'-resize','320x224!','-alpha','off','-depth','8','rgb:-'])
A=raw(sys.argv[1]);B=raw(sys.argv[2])
for c,name in enumerate('RGB'):
    bins={}
    for i in range(c,len(A),3):
        bins.setdefault(B[i]//32,[]).append(A[i])
    print(name,' '.join('%d:%d'%(k*32+16,sorted(v)[len(v)//2]) for k,v in sorted(bins.items()) if len(v)>30))
