; Standalone x86 split-stream unfilter — reverses rekkrunchy's -cx filter.
; Derived from ryg's depack2.asm (dispack decoder); the Win32 import-processing
; trailer is removed and the addressing is adapted to decode into a caller-given
; buffer instead of in-place at the image VA.
;
; cdecl: int rekk_unfilter(const uint8_t *packed, uint8_t *dst) -> decoded size
;
; Input layout (produced by X86Filter): [va:4][20 stream sizes:4][20 streams].
; The filter encodes jump/call targets as file offsets, so `va` matters only for
; jump-table detection: we bias the stored jump-table address into dst-space so
; the hot compare stays a single `cmp edi,[jumpTable]`.

bits 32
global rekk_unfilter

%define NBUFFERS 26
%define BUFFER dataArea.buffer

struc dataArea
.buffer     resd NBUFFERS
.offset     resd 1          ; origdst - 4 (for funcTable / rel32 reconstruction)
.nextFunc   resd 1
.jumpTable  resd 1          ; next jump-table address, biased into dst-space
.jtbias     resd 1          ; origdst - va  (adds VA-space addr -> dst pointer)
.codebuf    resb 1
.modrmbuf   resb 1
._pad       resb 2
.funcTable  resd 256
.size:
endstruc

%define fMODE 0x3
%define fMO   0x3
%define fMR   0x2
%define fAM   0x1
%define fNI   0x0
%define fBI   0x4
%define fDI   0x8

section .bss
WorkArea resb dataArea.size

section .text

rekk_unfilter:
  pushad
  mov       ebp, WorkArea
  mov       esi, [esp+36]            ; arg1 packed
  mov       edi, [esp+40]            ; arg2 dst

  lodsd                             ; va
  mov       ebx, edi                 ; ebx = origdst
  sub       ebx, eax                 ; ebx = origdst - va = jtbias
  mov       [ebp+dataArea.jtbias], ebx

  lea       ebx, [edi-4]
  mov       [ebp+dataArea.offset], ebx

  xor       eax, eax
  dec       eax                              ; eax = ~0
  mov       [ebp+dataArea.jumpTable], eax    ; ~0 sentinel (no pending table)
  neg       eax                              ; eax = 1
  mov       [ebp+dataArea.nextFunc], eax
  mov       [ebp+dataArea.funcTable], eax    ; funcTablePos starts at 1

  ; set up the stream cursors: buffer[i] = streams_base + sum(sizes[0..i-1])
  lea       ebx, [esi+NBUFFERS*4]
  xor       ecx, ecx
.init:
  lodsd
  mov       [ebp+BUFFER+ecx*4], ebx
  add       ebx, eax
  inc       ecx
  cmp       cl, NBUFFERS
  jne       .init

  ; buffer[0] walks the opcode stream; it finishes when it reaches buffer[1].
  mov       esi, [ebp+BUFFER+1*4]
  xchg      esi, [ebp+BUFFER]

.main:
  cmp       esi, [ebp+BUFFER]
  je        .done

  cmp       edi, [ebp+dataArea.jumpTable]
  jb        .gotins
  jne       .jtclr

  ; emit a jump table: [count][count dwords] copied verbatim from stream 15
  xchg      esi, [ebp+BUFFER+15*4]
  lodsd
  xchg      eax, ecx
  rep       movsd
  xchg      esi, [ebp+BUFFER+15*4]

.jtclr:
  or        dword [ebp+dataArea.jumpTable], byte -1
  jmp       .main

.done:
  lea       eax, [edi-4]             ; offset = origdst-4, so edi-4-offset = edi-origdst
  sub       eax, [ebp+dataArea.offset]
  mov       [esp+28], eax            ; popad returns it in eax
  popad
  ret

.gotins:
  xor       eax, eax
  cmp       [ebp+dataArea.nextFunc], eax
  lodsb
  je        .testes
  cmp       al, 0xcc                 ; int3 padding never starts a function
  je        .testes

  lea       ecx, [edi-4]
  sub       ecx, [ebp+dataArea.offset]   ; ecx = memory (current offset)
  mov       ebx, [ebp+dataArea.funcTable]
  mov       [ebp+dataArea.funcTable+ebx*4], ecx
.incm:
  inc       byte [ebp+dataArea.funcTable]
  jz        .incm
  mov       byte [ebp+dataArea.nextFunc], 0

.testes:
  cmp       al, 0xce                 ; escape -> literal byte
  jne       .noesc
  movsb
  jmp       .main

.noesc:
  stosb
  xor       edx, edx
  cmp       al, 0x66                 ; operand-size prefix
  jne       .nopfx
  mov       dh, 1
  lodsb
  stosb
.nopfx:
  mov       [ebp+dataArea.codebuf], al
  mov       bl, al
  cmp       bl, 0xcc
  je        .isret
  sub       bl, 0xc2
  cmp       bl, 1
  ja        .noret
.isret:
  mov       byte [ebp+dataArea.nextFunc], 1
.noret:
  mov       ebx, Tables              ; xlatb base (al = code>>1 indexes the table)
  cmp       al, 0x0f                 ; two-byte opcode
  jne       .notwo
  lodsb
  stosb
  mov       ah, 1
.notwo:
  shr       eax, 1                   ; flags = Table[code>>1], nibble by code&1
  xlatb
  jnc       .flagok
  shr       al, 4
.flagok:
  and       al, 0xf
  mov       cl, al

  test      cl, fMR                  ; modrm?
  jz        .nomdrm
  lodsb
  stosb
  mov       [ebp+dataArea.modrmbuf], al
  mov       ch, al

  mov       al, cl
  and       al, fMODE
  cmp       al, fMO                  ; group opcode: resolve immediate type
  jne       .noxtra
  mov       cl, fMR|fNI
  test      ch, 0x38
  jnz       .noxtra
  mov       bl, [edi-2]
  test      bl, 0x08
  jnz       .noxtra
  add       cl, fBI
  test      bl, 0x01
  jz        .noxtra
  add       cl, fDI-fBI
.noxtra:
  and       ch, 0xc7
  cmp       ch, 0xc4
  je        .nosib
  mov       al, ch
  and       al, 0x07
  cmp       al, 0x04                 ; sib present?
  jne       .nosib
  xchg      esi, [ebp+BUFFER+19*4]
  movsb
  xchg      esi, [ebp+BUFFER+19*4]
.nosib:
  mov       dl, ch
  and       dl, 0xc0
  cmp       dl, 0x40                 ; disp8 (per base register stream)
  jne       .nodis8
  movzx     ebx, ch
  and       bl, 0x07
  xchg      esi, [ebp+BUFFER+1*4+ebx*4]
  movsb
  xchg      esi, [ebp+BUFFER+1*4+ebx*4]
.nodis8:
  cmp       dl, 0x80
  je        .dis32
  cmp       ch, 0x05
  je        .dis32
  test      dl, dl
  jnz       .nomdrm
  mov       al, [edi-1]
  and       al, 0x07
  cmp       al, 0x05
  jne       .nomdrm
.dis32:
  ; pick disp32 stream: (modrm&0xc7)==5 -> 14 ; (modrm&7)==4 (SIB) -> 24 ; else 13
  mov       ebx, 13
  cmp       ch, 5
  jne       .d32nomr5
  mov       bl, 14
  jmp       short .d32sel
.d32nomr5:
  mov       al, ch
  and       al, 7
  cmp       al, 4
  jne       .d32sel
  mov       bl, 24
.d32sel:
  xchg      esi, [ebp+BUFFER+ebx*4]
  lodsd
  xchg      esi, [ebp+BUFFER+ebx*4]
  bswap     eax
  stosd
  cmp       word [ebp+dataArea.codebuf], 0x24ff   ; jmp [table] -> note address
  jne       .nomdrm
  add       eax, [ebp+dataArea.jtbias]            ; VA-space -> dst pointer space
  cmp       eax, [ebp+dataArea.jumpTable]
  jae       .nomdrm
  mov       [ebp+dataArea.jumpTable], eax

.nomdrm:
  mov       al, cl
  and       al, fMODE
  cmp       al, fAM
  jne       .noaddr

  shr       cl, 2                    ; address-mode immediate type
  jnz       .noad
  ; fAD: absolute 4-byte address from stream 15 (stored big-endian)
  xchg      esi, [ebp+BUFFER+15*4]
  lodsd
  bswap     eax
  stosd
  xchg      esi, [ebp+BUFFER+15*4]
  jmp       short .tomain
.noad:
  dec       cl
  jnz       .dwdrl
  ; fBR: 1-byte relative from stream 9
  xchg      esi, [ebp+BUFFER+9*4]
  movsb
  xchg      esi, [ebp+BUFFER+9*4]
  jmp       short .tomain
.dwdrl:
  xor       ebx, ebx
  cmp       byte [edi-1], 0xe8       ; call rel32 vs jmp/jcc rel32
  je        .dwcal
  ; jmp/jcc: absolute target from stream 17 (no delta, big-endian)
  xchg      esi, [ebp+BUFFER+17*4]
  lodsd
  bswap     eax
  xchg      esi, [ebp+BUFFER+17*4]
  jmp       short .storad
.dwcal:
  xor       eax, eax
  xchg      esi, [ebp+BUFFER+16*4]
  lodsb                              ; funcTable index (0 = new target)
  xchg      esi, [ebp+BUFFER+16*4]
  test      al, al
  jz        .dcesc
  mov       eax, [ebp+dataArea.funcTable+eax*4]
  jmp       short .storad
.dcesc:
  xchg      esi, [ebp+BUFFER+18*4]
  lodsd                              ; new absolute target offset from stream 18 (BE)
  bswap     eax
  xchg      esi, [ebp+BUFFER+18*4]
  mov       ebx, [ebp+dataArea.funcTable]
  mov       [ebp+dataArea.funcTable+ebx*4], eax
.dcinc:
  inc       byte [ebp+dataArea.funcTable]
  jz        .dcinc
.storad:
  sub       eax, edi                 ; rel32 = target_offset - (edi - origdst) - 4
  add       eax, [ebp+dataArea.offset]
  stosd
.tomain:                             ; central trampoline: lets nearby copy-and-
  jmp       .main                    ; continue paths reach .main with jmp short

.noaddr:
  shr       cl, 2
  jz        near .main
  dec       cl
  jnz       .dwow
  ; fBI: 1-byte immediate, stream chosen by opcode class (see imm8_stream in C)
  call      .imm8sel                 ; ebx = stream index
  xchg      esi, [ebp+BUFFER+ebx*4]
  movsb
  xchg      esi, [ebp+BUFFER+ebx*4]
  jmp       short .tomain
.dwow:
  dec       cl
  jnz       .word
  test      dh, dh                   ; o16? then it's a word immediate
  jnz       .word
  ; fDI: 4-byte immediate, stream chosen by opcode class; stored big-endian (bswap)
  call      .imm32sel                ; ebx = stream index
  xchg      esi, [ebp+BUFFER+ebx*4]
  lodsd
  bswap     eax
  stosd
  xchg      esi, [ebp+BUFFER+ebx*4]
  jmp       short .tomain
.word:
  ; fWI / o16 fDI: 2-byte immediate from stream 11
  xchg      esi, [ebp+BUFFER+11*4]
  movsw
  xchg      esi, [ebp+BUFFER+11*4]
  jmp       short .tomain

; imm8_stream: ebx = 20 if op in {80,83}; 21 if op in {04,0c,24,2c,34,3c}; else 10.
; Explicit compares to match C imm8_stream() exactly (cold path; clarity > size).
; op = current opcode (codebuf). Clobbers eax,edx,esi; preserves edi/ebp/ecx.
.imm8sel:
  mov       al, [ebp+dataArea.codebuf]
  mov       ebx, 20
  cmp       al, 0x80
  je        .i8done
  cmp       al, 0x83
  je        .i8done
  mov       bl, 21
  push      esi
  mov       esi, .alimm8             ; table of the 6 AL-imm8 opcodes, 0-terminated
.i8scan:
  mov       dl, [esi]
  inc       esi
  test      dl, dl
  jz        .i8rest
  cmp       dl, al
  jne       .i8scan
  pop       esi
  ret                                ; matched -> ebx=21
.i8rest:
  pop       esi
  mov       bl, 10
.i8done:
  ret
.alimm8 db 0x04,0x0c,0x24,0x2c,0x34,0x3c,0

; imm32_stream: ebx = 22 if op==c7; 23 if b8..bf; 25 if op==0x68 (push); else 12.
.imm32sel:
  mov       al, [ebp+dataArea.codebuf]
  mov       ebx, 22
  cmp       al, 0xc7
  je        .i32done
  mov       bl, 23
  mov       ah, al
  and       ah, 0xf8
  cmp       ah, 0xb8                  ; b8..bf
  je        .i32done
  mov       bl, 25
  cmp       al, 0x68                  ; push imm32
  je        .i32done
  mov       bl, 12
.i32done:
  ret

section .data
; flag table: 2 nibbles per byte (code>>1 selects byte, code&1 selects nibble),
; matching X86Filter's Table0/Table0f packing. 'PR'/'DEPACKTABLE' markers keep
; the layout identical to the original dispack table.
Tables:
  db 34,34,132,0,34,34,132,0,34,34,132,0,34,34,132,0
  db 34,34,132,0,34,34,132,0,34,34,132,0,34,34,132,0
  db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
  db 0,34,0,0,168,100,0,0,85,85,85,85,85,85,85,85
  db 166,102,34,34,34,34,34,34,0,0,0,0,0,0,0,0
  db 17,17,0,0,132,0,0,0,68,68,68,68,136,136,136,136
  db 102,12,34,166,0,12,64,0,34,34,68,0,34,34,34,34
  db 85,85,68,68,221,81,0,0,0,0,0,51,0,0,0,51
  db 0,0,0,0,0,0,0,0,34,34,34,34,0,0,0,0
  db 34,34,0,0,34,34,34,34,0,0,0,0,0,0,0,0
  db 34,34,34,34,34,34,34,34,34,34,34,34,34,34,34,34
  db 34,34,34,34,34,34,34,34,102,102,34,2,0,0,0,34
  db 221,221,221,221,221,221,221,221,34,34,34,34,34,34,34,34
  db 0,32,38,34,0,32,38,32,34,34,34,34,0,32,34,34
  db 34,34,34,34,0,0,0,0,34,34,34,34,34,34,34,34
  db 34,34,34,34,34,34,34,34,34,34,34,34,34,34,34,2
  times (256-($-Tables)) db 0

section .note.GNU-stack noalloc noexec nowrite progbits
