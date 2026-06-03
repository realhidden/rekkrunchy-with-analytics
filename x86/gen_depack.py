#!/usr/bin/env python3
# Derive a standalone, cdecl x86 self-decompressor (rekk_depack) from the
# original depacker.asm by replacing only its PE-specific entry/exit. The model
# and range-decode loop are kept verbatim, so it stays byte-exact with the C
# codec. Output: x86/depack.asm.
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / "depacker.asm").read_text()

# 1. PE fixup symbol block -> single global.
src = src.replace(
"""; ---- fixups

extern        PACKED
extern        DEPACKED
extern        DEPACKEDSIZE
extern        IMPORTS
extern        LOADLIBRARY

extern        PATCHOFFS
extern        PATCHVALUE

global        STAGE0ENTRY
global        DEXORENTRY

extern        PACKEDSIZE""",
"""; ---- entry

global        rekk_depack""")

# 2. Entry prologue: read (src,dst) cdecl args, read 4-byte LE size header.
src = src.replace(
"""STAGE0ENTRY:
  mov       ebp, WorkData
  mov				dword [ebp+Work.src], PACKED
  mov       eax, DEPACKED
  mov				dword [ebp+Work.dst], eax
  mov       dword [ebp+Work.origdst], eax
  push      eax
  mov				dword [ebp+Work.outsize], DEPACKEDSIZE""",
"""rekk_depack:                       ; cdecl int rekk_depack(const u8 *src, u8 *dst)
  pushad
  mov       ebp, WorkData
  mov       esi, [esp+36]            ; arg1 src (pushad=32 + ret=4)
  mov       edi, [esp+40]            ; arg2 dst
  lodsd                              ; eax = 4-byte LE size; esi advances past it
  mov       dword [ebp+Work.outsize], eax
  mov       dword [ebp+Work.src], esi
  mov       dword [ebp+Work.dst], edi
  mov       dword [ebp+Work.origdst], edi""")

# 3. Done handler: drop PE import processing; return decoded size via popad.
done_old = src[src.index(".decodedone:"):src.index(".decodenobytedone:")]
done_new = """.decodedone:
  emms
  mov       eax, [ebp+Work.dst]
  sub       eax, [ebp+Work.origdst]  ; decoded byte count
  mov       [esp+28], eax            ; overwrite saved EAX so popad returns it
  popad
  ret

"""
src = src.replace(done_old, done_new)

# 4. Remove the PE DEXORENTRY trampoline.
src = src.replace("""DEXORENTRY:
jmp STAGE0ENTRY

""", "")

# 5. The original keeps its helper routines (train/contextHash/squash/decodebit)
# plus their small read-only tables under `section .data`. That is fine for a PE
# image but on Linux ELF `.data` is non-executable, so calling into it faults.
# Put that block in `.text` (it is read-only code + const tables).
src = src.replace(
"""; ---- initialized data

section       .data""",
"""; ---- helper routines + const tables (kept executable: .text, not .data)

section       .text""")

# 6. NASM elf needs a non-exec stack note to avoid linker warnings.
src += "\nsection .note.GNU-stack noalloc noexec nowrite progbits\n"

out = root / "x86" / "depack.asm"
out.write_text(src)
print("wrote", out)
