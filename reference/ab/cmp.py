#!/usr/bin/env python3
# cmp.py <a.png> <b.png> [out_prefix]: both cropped to their 4:3 picture and resized to 640x448; PSNR (whole, and masks)
import sys,subprocess,re
def norm(p,o):
    w,h=map(int,subprocess.check_output(['identify','-format','%w %h',p]).split())
    if w*3>h*4: cw,ch=h*4//3,h
    else: cw,ch=w,w*3//4
    subprocess.check_call(['magick',p,'-gravity','center','-crop',f'{cw}x{ch}+0+0','+repage','-filter','Triangle','-resize','640x448!','-alpha','off',o])
a,b=sys.argv[1:3]; pre=sys.argv[3] if len(sys.argv)>3 else '/tmp/claude-1000/ab'
norm(a,pre+'_a.png'); norm(b,pre+'_b.png')
r=subprocess.run(['magick','compare','-metric','PSNR',pre+'_a.png',pre+'_b.png',pre+'_d.png'],capture_output=True,text=True)
print(r.stderr.split('(')[0].strip())
