#!/usr/bin/env python3
# usage: ramlist.py ref.bin ours.bin start end [max]: clusters of differing words with values
import sys,array
a=array.array('I');b=array.array('I')
a.frombytes(open(sys.argv[1],'rb').read());b.frombytes(open(sys.argv[2],'rb').read())
lo,hi=int(sys.argv[3],16)//4,int(sys.argv[4],16)//4; mx=int(sys.argv[5]) if len(sys.argv)>5 else 80
n=0;i=lo
while i<hi and n<mx:
    if a[i]!=b[i]:
        j=i
        while j+1<hi and (a[j+1]!=b[j+1] or (j+2<hi and a[j+2]!=b[j+2])): j+=1
        print(f'{i*4:06x}+{(j-i+1)*4:<5x} ref {a[i]:08x} ours {b[i]:08x}'+(f' | {a[i+1]:08x} {b[i+1]:08x}' if j>i else ''))
        n+=1;i=j+1
    else:i+=1
