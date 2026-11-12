/* win-lj37's frames.
 *
 * void f_fin(void (*fn)(void)):  __try { fn(); } __finally { lj_finally }
 *
 * int f_jmp_owner(jmp_buf jb, void (*fn)(void), int frame_mode):
 *     if (_setjmp(jb, frame) == 0) { __try { fn(); } __finally { lj_finally } return 0; }
 *     return 1;
 *   with a frame pointer at an offset (rbp = establisher + 0x20); the frame
 *   given to _setjmp: mode 0 the establisher frame itself, mode 1 rbp (no
 *   frame's establisher: a longjmp to it cannot find its target), mode 2
 *   g_high_frame (above every frame on the stack).
 *
 * int f_nv(jmp_buf jb, const uint64_t in[29], uint64_t out[29], void (*fn)(void)):
 *   rbx rsi rdi rbp r12-r15 (in[0..7]), xmm6-15 (in[8..28), two words
 *   each), MXCSR and the x87 control word (in[28]'s low and next halves)
 *   from in[], then _setjmp(jb, its establisher frame -- or 0 when g_nv_mode
 *   is 1); 0: call fn (which clobbers them all and longjmps back); else: all
 *   of them into out[].
 *
 * uint64_t c_outer(void) -> c_mid -> c_raiser(): frames for the consolidation
 * checks (as win-exc33's outer_frame and mid_frame): c_outer holds a pattern
 * in rbx and xmm6 across its call, c_mid clobbers both; an unwind that lands
 * at c_outer_landing stores what they hold there. Their handlers are lj37.c's
 * h_outer and h_mid. */

	.text
	.p2align 4
	.globl	f_fin
	.def	f_fin;	.scl	2;	.type	32;	.endef
	.seh_proc	f_fin
f_fin:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
.Lf_b:
	call	*%rcx
	nop
.Lf_e:
	nop
	addq	$0x28, %rsp
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	1
	.rva	.Lf_b
	.rva	.Lf_e
	.rva	lj_finally
	.long	0
	.text
	.seh_endproc

	.p2align 4
	.globl	f_jmp_owner
	.def	f_jmp_owner;	.scl	2;	.type	32;	.endef
	.seh_proc	f_jmp_owner
f_jmp_owner:
	pushq	%rbp
	.seh_pushreg	%rbp
	pushq	%rbx
	.seh_pushreg	%rbx
	pushq	%rsi
	.seh_pushreg	%rsi
	subq	$0x40, %rsp
	.seh_stackalloc	0x40
	leaq	0x20(%rsp), %rbp
	.seh_setframe	%rbp, 0x20
	.seh_endprologue
	movq	%rcx, %rbx
	movq	%rdx, %rsi
	movq	%rbx, %rcx
	cmpl	$2, %r8d
	je	3f
	testl	%r8d, %r8d
	jnz	1f
	leaq	-0x20(%rbp), %rdx		/* the establisher frame */
	jmp	2f
1:	movq	%rbp, %rdx			/* not a frame's establisher */
	jmp	2f
3:	movq	g_high_frame(%rip), %rdx	/* above every frame */
2:	call	_setjmp
	testl	%eax, %eax
	jnz	.Lo_back
.Lo_b:
	call	*%rsi
	nop
.Lo_e:
	nop
	xorl	%eax, %eax
	jmp	.Lo_out
.Lo_back:
	movl	$1, %eax
.Lo_out:
	leaq	0x20(%rbp), %rsp
	popq	%rsi
	popq	%rbx
	popq	%rbp
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	1
	.rva	.Lo_b
	.rva	.Lo_e
	.rva	lj_finally
	.long	0
	.text
	.seh_endproc

	.p2align 4
	.globl	f_nv
	.def	f_nv;	.scl	2;	.type	32;	.endef
	.seh_proc	f_nv
f_nv:
	pushq	%rbp
	.seh_pushreg	%rbp
	pushq	%rbx
	.seh_pushreg	%rbx
	pushq	%rsi
	.seh_pushreg	%rsi
	pushq	%rdi
	.seh_pushreg	%rdi
	pushq	%r12
	.seh_pushreg	%r12
	pushq	%r13
	.seh_pushreg	%r13
	pushq	%r14
	.seh_pushreg	%r14
	pushq	%r15
	.seh_pushreg	%r15
	subq	$0xC8, %rsp
	.seh_stackalloc	0xC8
	movaps	%xmm6, 0x20(%rsp)
	.seh_savexmm	%xmm6, 0x20
	movaps	%xmm7, 0x30(%rsp)
	.seh_savexmm	%xmm7, 0x30
	movaps	%xmm8, 0x40(%rsp)
	.seh_savexmm	%xmm8, 0x40
	movaps	%xmm9, 0x50(%rsp)
	.seh_savexmm	%xmm9, 0x50
	movaps	%xmm10, 0x60(%rsp)
	.seh_savexmm	%xmm10, 0x60
	movaps	%xmm11, 0x70(%rsp)
	.seh_savexmm	%xmm11, 0x70
	movaps	%xmm12, 0x80(%rsp)
	.seh_savexmm	%xmm12, 0x80
	movaps	%xmm13, 0x90(%rsp)
	.seh_savexmm	%xmm13, 0x90
	movaps	%xmm14, 0xA0(%rsp)
	.seh_savexmm	%xmm14, 0xA0
	movaps	%xmm15, 0xB0(%rsp)
	.seh_savexmm	%xmm15, 0xB0
	.seh_endprologue
	movq	%r8, 0xC0(%rsp)			/* out */
	movq	%r9, %rax			/* fn, kept in in[] order's way below */
	movq	%rdx, %r10			/* in */
	movq	0x00(%r10), %rbx
	movq	0x08(%r10), %rsi
	movq	0x10(%r10), %rdi
	movq	0x18(%r10), %rbp
	movq	0x20(%r10), %r12
	movq	0x28(%r10), %r13
	movq	0x30(%r10), %r14
	movq	0x38(%r10), %r15
	movups	0x40(%r10), %xmm6
	movups	0x50(%r10), %xmm7
	movups	0x60(%r10), %xmm8
	movups	0x70(%r10), %xmm9
	movups	0x80(%r10), %xmm10
	movups	0x90(%r10), %xmm11
	movups	0xA0(%r10), %xmm12
	movups	0xB0(%r10), %xmm13
	movups	0xC0(%r10), %xmm14
	movups	0xD0(%r10), %xmm15
	ldmxcsr	0xE0(%r10)
	fldcw	0xE4(%r10)
	movq	%rax, g_nv_fn(%rip)
	xorl	%edx, %edx			/* g_nv_mode 1: no frame (0) */
	cmpl	$0, g_nv_mode(%rip)
	jne	1f
	movq	%rsp, %rdx			/* g_nv_mode 0: the establisher frame */
1:	call	_setjmp
	testl	%eax, %eax
	jnz	.Lnv_back
	call	*g_nv_fn(%rip)
	nop
.Lnv_back:
	movq	0xC0(%rsp), %r10		/* eax: what setjmp returned */
	movq	%rbx, 0x00(%r10)
	movq	%rsi, 0x08(%r10)
	movq	%rdi, 0x10(%r10)
	movq	%rbp, 0x18(%r10)
	movq	%r12, 0x20(%r10)
	movq	%r13, 0x28(%r10)
	movq	%r14, 0x30(%r10)
	movq	%r15, 0x38(%r10)
	movups	%xmm6, 0x40(%r10)
	movups	%xmm7, 0x50(%r10)
	movups	%xmm8, 0x60(%r10)
	movups	%xmm9, 0x70(%r10)
	movups	%xmm10, 0x80(%r10)
	movups	%xmm11, 0x90(%r10)
	movups	%xmm12, 0xA0(%r10)
	movups	%xmm13, 0xB0(%r10)
	movups	%xmm14, 0xC0(%r10)
	movups	%xmm15, 0xD0(%r10)
	stmxcsr	0xE0(%r10)
	fnstcw	0xE4(%r10)
	movaps	0x20(%rsp), %xmm6
	movaps	0x30(%rsp), %xmm7
	movaps	0x40(%rsp), %xmm8
	movaps	0x50(%rsp), %xmm9
	movaps	0x60(%rsp), %xmm10
	movaps	0x70(%rsp), %xmm11
	movaps	0x80(%rsp), %xmm12
	movaps	0x90(%rsp), %xmm13
	movaps	0xA0(%rsp), %xmm14
	movaps	0xB0(%rsp), %xmm15
	addq	$0xC8, %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rdi
	popq	%rsi
	popq	%rbx
	popq	%rbp
	ret
	.seh_endproc

/* void f_clobber_jump(void): every nonvolatile register to garbage, then
 * longjmp(g_nv_jb, 1). */
	.p2align 4
	.globl	f_clobber_jump
	.def	f_clobber_jump;	.scl	2;	.type	32;	.endef
	.seh_proc	f_clobber_jump
f_clobber_jump:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
	movq	$-1, %rbx
	movq	$-1, %rsi
	movq	$-1, %rdi
	movq	$-1, %rbp
	movq	$-1, %r12
	movq	$-1, %r13
	movq	$-1, %r14
	movq	$-1, %r15
	pcmpeqd	%xmm6, %xmm6
	pcmpeqd	%xmm7, %xmm7
	pcmpeqd	%xmm8, %xmm8
	pcmpeqd	%xmm9, %xmm9
	pcmpeqd	%xmm10, %xmm10
	pcmpeqd	%xmm11, %xmm11
	pcmpeqd	%xmm12, %xmm12
	pcmpeqd	%xmm13, %xmm13
	pcmpeqd	%xmm14, %xmm14
	pcmpeqd	%xmm15, %xmm15
	ldmxcsr	g_clob_csr(%rip)
	fldcw	g_clob_cw(%rip)
	leaq	g_nv_jb(%rip), %rcx
	movl	$1, %edx
	call	longjmp
	int3
	.seh_endproc

	.p2align 4
	.globl	c_outer
	.def	c_outer;	.scl	2;	.type	32;	.endef
	.seh_proc	c_outer
c_outer:
	pushq	%rbx
	.seh_pushreg	%rbx
	subq	$0x40, %rsp
	.seh_stackalloc	0x40
	movaps	%xmm6, 0x20(%rsp)
	.seh_savexmm	%xmm6, 0x20
	.seh_endprologue
	movabsq	$0xB0B0B0B0B0B0, %rbx
	movq	%rbx, %xmm6
	call	c_mid
	nop
	jmp	.Lco_done
	.globl	c_outer_landing
c_outer_landing:
	movq	%rbx, g_landing_rbx(%rip)
	movq	%xmm6, g_landing_xmm6(%rip)
.Lco_done:
	movaps	0x20(%rsp), %xmm6
	addq	$0x40, %rsp
	popq	%rbx
	ret
	.seh_handler	h_outer, @except, @unwind
	.seh_endproc

	.p2align 4
	.def	c_mid;	.scl	3;	.type	32;	.endef
	.seh_proc	c_mid
c_mid:
	pushq	%rbp
	.seh_pushreg	%rbp
	pushq	%rbx
	.seh_pushreg	%rbx
	subq	$0x48, %rsp
	.seh_stackalloc	0x48
	leaq	0x20(%rsp), %rbp
	.seh_setframe	%rbp, 0x20
	movaps	%xmm6, 0x30(%rsp)
	.seh_savexmm	%xmm6, 0x30
	.seh_endprologue
	movq	$0x1111, %rbx
	movq	%rbx, %xmm6
	call	c_raiser
	nop
	movaps	0x10(%rbp), %xmm6
	leaq	0x28(%rbp), %rsp
	popq	%rbx
	popq	%rbp
	ret
	.seh_handler	h_mid, @except, @unwind
	.seh_endproc

	.data
	.p2align 3
g_nv_fn:
	.quad	0
g_clob_csr:
	.long	0x7F80			/* round toward zero, all masked */
g_clob_cw:
	.word	0x0E7F			/* round toward zero, 53-bit, all masked */
