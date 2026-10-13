#!/usr/bin/env python3
"""Execute the production ring planner against CREATE_VERTEX's allocation ABI."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'parhelia_context.c').read_text()
a=s.index('static unsigned\nph_vertex_ring_slot_count')
b=s.index('\nstatic void\nph_create_vertex_ring',a)
code=r"""
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define PH_VRAM_VERTEX_LEGACY_SLOTS 3
#define PH_VRAM_VERTEX_SLOTS_MAX 16
#define MIN2(a,b) ((a)<(b)?(a):(b))
"""+s[a:b]+r"""
int main(void) {
 assert(ph_vertex_ring_slot_count(16,1024*3*10*4)==8);
 assert(ph_vertex_ring_slot_count(8,1024*3*10*4)==7);
 assert(ph_vertex_ring_slot_count(3,221*3*10*4)==3);
 assert(!ph_vertex_ring_slot_count(16,0));
 assert(!ph_vertex_ring_slot_count(16,(1u<<20)+1));
 for(unsigned q=0;q<32;q++)for(unsigned packets=1;packets<=1024;packets++) {
  uint64_t bytes=packets*3*10*4;
  unsigned n=ph_vertex_ring_slot_count(q,bytes);
  unsigned wanted=q>3&&q<=16?q-1:3;
  assert(n>0&&n<=wanted&&n*bytes<=(1u<<20));
  assert(n==wanted||(n+1)*bytes>(1u<<20));
 }
 puts("PASS actual ring planner: 32768 cases, 16-slot queue fits eight full buffers within1MiB ABI");
}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
