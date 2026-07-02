; mathieeedoubtrans.library -- minimal FPU-based replacement
;
; Written from scratch for the RustChain Amiga devkit (Elyan Labs, 2026).
; Not Commodore code, not AROS code. License: MIT.
;
; Companion to mathieeedoubbas.asm (see that file for the full rationale).
; Transcendental functions use 6888x instructions (fsin, fetox, flogn...).
; On a REAL 68040/68060 those are unimplemented in hardware and require the
; 68040/68060.library FPSP; under FS-UAE's FPU emulation they just work.
; fsqrt, fcmp, ftst, fmove and friends are native everywhere.
;
; Register conventions (from the official .fd file):
;   IEEEDPPow(exp,arg): exponent in D2/D3, argument in D0/D1, returns arg^exp
;   IEEEDPSincos(pf2,parm): A0 = pointer receiving cos, D0/D1 = angle
;
; Build:
;   vasmm68k_mot -quiet -Fhunk -m68020 -m68881 mathieeedoubtrans.asm -o mathieeedoubtrans.o
;   vlink -bamigahunk -x -nostdlib mathieeedoubtrans.o -o mathieeedoubtrans.library

        section "CODE",code

progstart:
        moveq   #-1,d0
        rts

        cnop    0,4
romtag:
        dc.w    $4afc           ; RTC_MATCHWORD
        dc.l    romtag
        dc.l    endskip
        dc.b    $80             ; RTF_AUTOINIT
        dc.b    40
        dc.b    9               ; NT_LIBRARY
        dc.b    0
        dc.l    libname
        dc.l    libid
        dc.l    inittab

        cnop    0,4
inittab:
        dc.l    36
        dc.l    functable
        dc.l    0
        dc.l    initfunc

functable:
        dc.l    LibOpen         ; -6
        dc.l    LibClose        ; -12
        dc.l    LibExpunge      ; -18
        dc.l    LibNull         ; -24
        dc.l    TAtan           ; -30
        dc.l    TSin            ; -36
        dc.l    TCos            ; -42
        dc.l    TTan            ; -48
        dc.l    TSincos         ; -54
        dc.l    TSinh           ; -60
        dc.l    TCosh           ; -66
        dc.l    TTanh           ; -72
        dc.l    TExp            ; -78
        dc.l    TLog            ; -84
        dc.l    TPow            ; -90
        dc.l    TSqrt           ; -96
        dc.l    TTieee          ; -102
        dc.l    TFieee          ; -108
        dc.l    TAsin           ; -114
        dc.l    TAcos           ; -120
        dc.l    TLog10          ; -126
        dc.l    -1

initfunc:                       ; d0=libBase a0=seglist a6=ExecBase
        move.l  d0,a1
        move.b  #9,8(a1)        ; ln_Type = NT_LIBRARY
        move.l  #libname,10(a1) ; ln_Name
        move.b  #6,14(a1)       ; lib_Flags
        move.w  #40,20(a1)      ; lib_Version
        move.w  #1,22(a1)       ; lib_Revision
        move.l  #libid,24(a1)   ; lib_IdString
        rts

LibOpen:
        addq.w  #1,32(a6)
        bclr    #3,14(a6)
        move.l  a6,d0
        rts
LibClose:
        subq.w  #1,32(a6)
LibExpunge:
LibNull:
        moveq   #0,d0
        rts

;----------------------------------------------------------------------
; Single-double-argument pattern: arg in D0/D1, result in D0/D1.
;----------------------------------------------------------------------

TSin:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fsin.x  fp0
ret1:   fmove.d fp0,(a7)
        movem.l (a7)+,d0-d1
        rts

TCos:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fcos.x  fp0
        bra.w   ret1

TTan:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        ftan.x  fp0
        bra.w   ret1

TAtan:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fatan.x fp0
        bra.w   ret1

TAsin:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fasin.x fp0
        bra.w   ret1

TAcos:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        facos.x fp0
        bra.w   ret1

TSinh:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fsinh.x fp0
        bra.w   ret1

TCosh:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fcosh.x fp0
        bra.w   ret1

TTanh:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        ftanh.x fp0
        bra.w   ret1

TExp:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fetox.x fp0
        bra.w   ret1

TLog:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        flogn.x fp0
        bra.w   ret1

TLog10:
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        flog10.x fp0
        bra.w   ret1

TSqrt:                          ; native FPU instruction on 040/060 too
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fsqrt.x fp0
        bra.w   ret1

TSincos:                        ; a0 = ptr for cos result, d0/d1 = angle
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fsincos.x fp0,fp1:fp0   ; sin -> fp0, cos -> fp1
        fmove.d fp1,(a0)
        bra.w   ret1

TPow:                           ; exp in d2/d3, arg in d0/d1 -> arg^exp
        movem.l d0-d3,-(a7)
        fmove.d (a7),fp0        ; arg
        flogn.x fp0             ; ln(arg)
        fmul.d  8(a7),fp0       ; * exp
        fetox.x fp0             ; e^(exp*ln(arg))
        fmove.d fp0,(a7)
        movem.l (a7),d0-d1
        lea     16(a7),a7
        rts

TTieee:                         ; double d0/d1 -> single float in d0
        movem.l d0-d1,-(a7)
        fmove.d (a7),fp0
        fmove.s fp0,(a7)
        move.l  (a7)+,d0
        addq.l  #4,a7
        rts

TFieee:                         ; single float in d0 -> double d0/d1
        move.l  d0,-(a7)
        fmove.s (a7)+,fp0
        fmove.d fp0,-(a7)
        movem.l (a7)+,d0-d1
        rts

libname: dc.b   'mathieeedoubtrans.library',0
libid:   dc.b   'mathieeedoubtrans 40.1 Elyan FPU replacement (02.07.2026)',13,10,0
        cnop    0,4
endskip:
