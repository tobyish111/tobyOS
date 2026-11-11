/* win-seh33's frames -- hand-written so their unwind information (and the
 * handlers attached to them) is exactly known. outer_frame and mid_frame each
 * carry a language handler of seh33.c's; mid_frame has a frame pointer and
 * saves xmm6, so an unwind through it exercises UWOP_SET_FPREG and
 * UWOP_SAVE_XMM128; c_frame carries __C_specific_handler with a scope table
 * of two scopes: a __finally nested in an __except. */

	.text

/* uint64_t outer_frame(void): rbx and xmm6 hold a pattern across the call to
 * mid_frame; an unwind that lands at outer_landing stores what they hold
 * there (the unwinder must have restored them from mid_frame's saves). */
	.p2align 4
	.globl	outer_frame
	.def	outer_frame;	.scl	2;	.type	32;	.endef
	.seh_proc	outer_frame
outer_frame:
	pushq	%rbx
	.seh_pushreg	%rbx
	subq	$0x40, %rsp
	.seh_stackalloc	0x40
	movaps	%xmm6, 0x20(%rsp)
	.seh_savexmm	%xmm6, 0x20
	.seh_endprologue
	movabsq	$0xB0B0B0B0B0B0, %rbx
	movq	%rbx, %xmm6
	call	mid_frame
	nop
	jmp	.Lout_done
	.globl	outer_landing
outer_landing:
	movq	%rbx, g_landing_rbx(%rip)
	movq	%xmm6, g_landing_xmm6(%rip)
.Lout_done:
	movaps	0x20(%rsp), %xmm6
	addq	$0x40, %rsp
	popq	%rbx
	ret
	.seh_handler	h_outer, @except, @unwind
	.seh_endproc

/* uint64_t mid_frame(void): clobbers rbx and xmm6, then calls g_mid_target
 * (raiser, or c_frame). mid_landing is both the normal path and an unwind
 * target; it returns rax. */
	.p2align 4
	.globl	mid_frame
	.def	mid_frame;	.scl	2;	.type	32;	.endef
	.seh_proc	mid_frame
mid_frame:
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
	call	*g_mid_target(%rip)
	nop
	.globl	mid_landing
mid_landing:
	movaps	0x10(%rbp), %xmm6
	leaq	0x28(%rbp), %rsp
	popq	%rbx
	popq	%rbp
	ret
	.seh_handler	h_mid, @except, @unwind
	.seh_endproc

/* uint64_t c_frame(void):
 *     __try {                       -- scope 1: __except (c_filter)
 *         __try { raiser(); }       -- scope 0: __finally (c_finally)
 *         __finally { c_finally }
 *     } __except (c_filter) { return exception code; }
 *     return 0;
 */
	.p2align 4
	.globl	c_frame
	.def	c_frame;	.scl	2;	.type	32;	.endef
	.seh_proc	c_frame
c_frame:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
.Lc_try_begin:
	call	raiser
	nop
.Lc_try_end:
	nop
	xorl	%eax, %eax
	jmp	.Lc_out
.Lc_except:
	nop
.Lc_out:
	addq	$0x28, %rsp
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	2
	.rva	.Lc_try_begin
	.rva	.Lc_try_end
	.rva	c_finally
	.long	0
	.rva	.Lc_try_begin
	.rva	.Lc_try_end
	.rva	c_filter
	.rva	.Lc_except
	.text
	.seh_endproc
