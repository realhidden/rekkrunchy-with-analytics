#!/usr/bin/env python3
# Patch the original model_asm.asm into a self-contained, cdecl-callable oracle
# used (a) to dump the init lookup tables and (b) as ground truth for the C-port
# differential test. Not shipped; dev/validation only.
import sys, pathlib

src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "model_asm.asm").read_text()

# BINARY mode (self-contained _bufPtr/_startBufPtr) + extra globals.
src = src.replace(
    'bits          32',
    'bits          32\n%define BINARY 1\n'
    'global model_init_c\nglobal model_c\nglobal get_workdata\n'
    'global get_offsets\nglobal set_bufptrs\n', 1)

src = src.replace(
    '%ifndef BINARY\nglobal @modelInitASM@0\nglobal @modelASM@4\n%endif',
    'global @modelInitASM@0\nglobal @modelASM@4')

src += '''

section .text

model_init_c:                 ; void model_init_c(void)
  call @modelInitASM@0
  ret

model_c:                      ; int model_c(int bit)
  mov ecx, [esp+4]
  call @modelASM@4
  ret

set_bufptrs:                  ; void set_bufptrs(void *buf, void *start)
  mov eax, [esp+4]
  mov [_bufPtr], eax
  mov eax, [esp+8]
  mov [_startBufPtr], eax
  ret

get_workdata:                 ; void* get_workdata(void)
  mov eax, WorkData
  ret

get_offsets:                  ; void get_offsets(int *out)
  mov edx, [esp+4]
  mov dword [edx+0],  Work.runTable
  mov dword [edx+4],  Work.stateCode
  mov dword [edx+8],  Work.stateNext
  mov dword [edx+12], Work.stateMap
  mov dword [edx+16], Work.stretch
  mov dword [edx+20], Work.APM
  mov dword [edx+24], Work.cm
  mov dword [edx+28], Work.size
  mov dword [edx+32], ContextModel.size
  mov dword [edx+36], Work.modelMem
  mov dword [edx+40], Work.tx
  mov dword [edx+44], Work.wx
  mov dword [edx+48], Work.tx2
  mov dword [edx+52], Work.wx2
  mov dword [edx+56], Work.pr
  mov dword [edx+60], Work.ctx
  mov dword [edx+64], Work.matchl
  mov dword [edx+68], Work.c0
  mov dword [edx+72], Work.bpos
  ret
'''
pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "/tmp/oracle.asm").write_text(src)
print("wrote oracle asm")
