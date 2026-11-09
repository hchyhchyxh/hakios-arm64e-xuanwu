#!/usr/bin/env python3
"""Check the actual interposer predicate against its documented boundary contract."""
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[3]
s=(r/'userland/userland/src/inject26.c').read_text()
start=s.index('big_pthread_create(')
f=s[start:s.index('/*\n * What the window server',start)]
predicate=re.search(r'if \((stack[^\n]+)\) \{',f).group(1)
c='#include <assert.h>\n#include <stddef.h>\n#define BIG_STACK_MIN (4u << 20)\nint main(void){size_t sizes[]={0,1u<<20,(4u<<20)-1,4u<<20,24u<<20,64u<<20};int expected[]={1,1,1,0,0,0};for(int i=0;i<6;i++){size_t stack=sizes[i];assert(('+predicate+')==expected[i]);}return 0;}\n'
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(c);subprocess.run(['cc','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
print('PASS: default and explicit small stacks grow; minimum, 24 MB and 64 MB stacks remain unchanged')
