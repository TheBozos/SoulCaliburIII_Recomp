#!/usr/bin/env python3
# What SC3 reads (bozo-calllog-tool calls <log> sceCdRead against a run with SC3_TRACE_CD set), by sectors and buffer: the LBAs differ (the reference ran from host files).
# usage: cd_compare.py <calls.txt> <run.log>
import re,sys,difflib
def ref(p): return [tuple(l.split()[4:6]) for l in open(p) if l.startswith('C ')]
def ours(p): return [tuple(m.group(1,2)) for m in (re.match(r'\[sc3:cd\] f\d+ \w+ (\w+) (\w+)',l) for l in open(p)) if m]
a,b=ref(sys.argv[1]),ours(sys.argv[2])
sm=difflib.SequenceMatcher(None,a,b,autojunk=False)
print(len(a),len(b),'ratio %.3f'%sm.ratio())
for t,i,j,k,l in sm.get_opcodes():
    if t!='equal': print(t,i,j,k,l,a[i:j][:8],b[k:l][:8])
