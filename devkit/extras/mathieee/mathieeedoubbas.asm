; mathieeedoubbas.library -- minimal FPU-based replacement
;
; Written from scratch for the RustChain Amiga devkit (Elyan Labs, 2026).
; Not Commodore code, not AROS code. License: MIT.
;
; Why this exists: the AROS m68k replacement ROM carries only
; mathieeesingbas.library. vbcc's vasm (built with -amiga-softfloat vclib)
; opens mathieeedoubbas.library and mathieeedoubtrans.library at startup,
; so a bare AROS ROM boot cannot run it. This library provides the classic
; mathieeedoubbas API on top of a 68881/68882-compatible FPU (the FS-UAE
; A4000/040 machine, or any real 68020+FPU / 68040 / 68060 Amiga).
;
; Register conventions (from the official .fd file):
;   doubles are passed in register PAIRS: hi word in D0, lo in D1
;   (second operand in D2/D3). Results return in D0/D1.
;
; Build (hosted vasm+vlink via vamos, or natively on the Amiga):
;   vasmm68k_mot -quiet -Fhunk -m68020 -m68881 mathieeedoubbas.asm -o mathieeedoubbas.o
;   vlink -bamigahunk -x -nostdlib mathieeedoubbas.o -o mathieeedoubbas.library

        section "CODE",code

; If someone runs the library file as a program, fail politely.
progstart:
        moveq   #-1,d0
        rts

        cnop    0,4
romtag:
        dc.w    $4afc           ; RTC_MATCHWORD
        dc.l    romtag          ; rt_MatchTag
        dc.l    endskip         ; rt_EndSkip
        dc.b    $80             ; rt_Flags  = RTF_AUTOINIT
        dc.b    40              ; rt_Version
        dc.b    9               ; rt_Type   = NT_LIBRARY
        dc.b    0               ; rt_Pri
        dc.l    libname         ; rt_Name
        dc.l    libid           ; rt_IdString
        dc.l    inittab         ; rt_Init

        cnop    0,4
inittab:
        dc.l    36              ; library base size (struct Library = 34)
        dc.l    functable       ; function table
        dc.l    0               ; no data init table (init func sets fields)
        dc.l    initfunc        ; init routine

functable:
        dc.l    LibOpen         ; -6
        dc.l    LibClose        ; -12
        dc.l    LibExpunge      ; -18
        dc.l    LibNull         ; -24
        dc.l    DPFix           ; -30
        dc.l    DPFlt           ; -36
        dc.l    DPCmp           ; -42
        dc.l    DPTst           ; -48
        dc.l    DPAbs           ; -54
        dc.l    DPNeg           ; -60
        dc.l    DPAdd           ; -66
        dc.l    DPSub           ; -72
        dc.l    DPMul           ; -78
        dc.l    DPDiv           ; -84
        dc.l    DPFloor         ; -90
        dc.l    DPCeil          ; -96
        dc.l    -1

initfunc:                       ; d0=libBase a0=seglist a6=ExecBase
        move.l  d0,a1
        move.b  #9,8(a1)        ; ln_Type     = NT_LIBRARY
        move.l  #libname,10(a1) ; ln_Name
        move.b  #6,14(a1)       ; lib_Flags   = LIBF_SUMUSED|LIBF_CHANGED
        move.w  #40,20(a1)      ; lib_Version
        move.w  #1,22(a1)       ; lib_Revision
        move.l  #libid,24(a1)   ; lib_IdString
        rts                     ; return libBase in d0

LibOpen:                        ; a6=libBase, d0=version
        addq.w  #1,32(a6)       ; lib_OpenCnt++
        bclr    #3,14(a6)       ; clear LIBF_DELEXP
        move.l  a6,d0
        rts
LibClose:
        subq.w  #1,32(a6)       ; lib_OpenCnt--
LibExpunge:
LibNull:
        moveq   #0,d0           ; never expunge (tiny, stays resident)
        rts

;----------------------------------------------------------------------
; Math functions. arg1 = D0/D1, arg2 = D2/D3, result = D0/D1.
; FP0/FP1 are scratch. D2+/A2+ are not modified.
;----------------------------------------------------------------------

DPAdd:
        movem.l d0-d3,-(a7)
        fmove.d (a7),fp0
        fadd.d  8(a7),fp0
ret2:   fmove.d fp0,(a7)
        movem.l (a7),d0-d1
        lea     16(a7),a7
        rts

DPSub:
        movem.l d0-d3,-(a7)
        fmove.d (a7),fp0
        fsub.d  8(a7),fp0
        bra.w   ret2

DPMul:
        movem.l d0-d3,-(a7)
        fmove.d (a7),fp0
        fmul.d  8(a7),fp0
        bra.w   ret2

DPDiv:
        movem.l d0-d3,-(a7)
        fmove.d (a7),fp0
        fdiv.d  8(a7),fp0
        bra.w   ret2

DPAbs:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fabs.x  fp0
ret1:   fmove.d fp0,(a7)
        movem.l (a7)+,d0-d1
        rts

DPNeg:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fneg.x  fp0
        bra.w   ret1

DPFlt:                          ; signed long in d0 -> double
        move.l  d0,-(a7)
        fmove.l (a7)+,fp0
        fmove.d fp0,-(a7)
        movem.l (a7)+,d0-d1
        rts

DPFix:                          ; double -> signed long, truncate toward 0
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fmove.l #$10,fpcr       ; rounding mode: toward zero
        fmove.l fp0,d0
        fmove.l #0,fpcr         ; restore round-to-nearest
        addq.l  #8,a7
        rts

DPCmp:                          ; arg1 <=> arg2 : d0 = -1 / 0 / +1
        movem.l d0-d3,-(a7)
        fmove.d (a7),fp0
        fcmp.d  8(a7),fp0
        lea     16(a7),a7
        fbgt.w  cmpgt
        fbeq.w  cmpeq
        moveq   #-1,d0
        rts
cmpgt:  moveq   #1,d0
        rts
cmpeq:  moveq   #0,d0
        rts

DPTst:                          ; arg <=> 0.0 : d0 = -1 / 0 / +1
        movem.l d0-d1,-(a7)
        fmove.d (a7)+,fp0
        ftst.x  fp0
        fbgt.w  cmpgt
        fbeq.w  cmpeq
        moveq   #-1,d0
        rts

DPFloor:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fmove.l #$20,fpcr       ; rounding mode: toward -infinity
        fint.x  fp0             ; note: FPSP-emulated on a real 68040
        fmove.l #0,fpcr
        bra.w   ret1

DPCeil:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fmove.l #$30,fpcr       ; rounding mode: toward +infinity
        fint.x  fp0
        fmove.l #0,fpcr
        bra.w   ret1

libname: dc.b   'mathieeedoubbas.library',0
libid:   dc.b   'mathieeedoubbas 40.1 Elyan FPU replacement (02.07.2026)',13,10,0
        cnop    0,4
endskip:
