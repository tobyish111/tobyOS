/* tobyrt.dll's assembly: capturing and restoring a CONTEXT, the two frames
 * the dispatcher and the unwinder call a language handler from, and the
 * memcpy/memset the C half's structure copies become.
 *
 * The x64 CONTEXT (winnt.h): ContextFlags 0x30, MxCsr 0x34, SegCs..SegSs
 * 0x38..0x42, EFlags 0x44, Rax 0x78, Rcx 0x80, Rdx 0x88, Rbx 0x90, Rsp 0x98,
 * Rbp 0xA0, Rsi 0xA8, Rdi 0xB0, R8..R15 0xB8..0xF0, Rip 0xF8, FltSave 0x100
 * (its ControlWord at +0, MxCsr at +0x18), Xmm0..Xmm15 0x1A0..0x290. */

	.text

/* void RtlCaptureContext(CONTEXT *ctx): the caller's registers as they are
 * when this call returns -- Rip the return address, Rsp the caller's. */
	.p2align 4
	.globl	rt_capture_context
	.def	rt_capture_context;	.scl	2;	.type	32;	.endef
	.seh_proc	rt_capture_context
rt_capture_context:
	pushfq
	.seh_stackalloc	8
	.seh_endprologue
	movq	%rax, 0x78(%rcx)
	movq	%rcx, 0x80(%rcx)
	movq	%rdx, 0x88(%rcx)
	movq	%rbx, 0x90(%rcx)
	movq	%rbp, 0xA0(%rcx)
	movq	%rsi, 0xA8(%rcx)
	movq	%rdi, 0xB0(%rcx)
	movq	%r8, 0xB8(%rcx)
	movq	%r9, 0xC0(%rcx)
	movq	%r10, 0xC8(%rcx)
	movq	%r11, 0xD0(%rcx)
	movq	%r12, 0xD8(%rcx)
	movq	%r13, 0xE0(%rcx)
	movq	%r14, 0xE8(%rcx)
	movq	%r15, 0xF0(%rcx)
	leaq	0x10(%rsp), %rax
	movq	%rax, 0x98(%rcx)
	movq	8(%rsp), %rax
	movq	%rax, 0xF8(%rcx)
	movl	(%rsp), %eax
	movl	%eax, 0x44(%rcx)
	movw	%cs, 0x38(%rcx)
	movw	%ds, 0x3A(%rcx)
	movw	%es, 0x3C(%rcx)
	movw	%fs, 0x3E(%rcx)
	movw	%gs, 0x40(%rcx)
	movw	%ss, 0x42(%rcx)
	stmxcsr	0x34(%rcx)
	testb	$15, %cl			/* W7d: an aligned CONTEXT takes the whole x87/SSE state */
	jnz	1f
	fxsave	0x100(%rcx)
	jmp	2f
1:	fnstcw	0x100(%rcx)
	stmxcsr	0x118(%rcx)
	movups	%xmm0, 0x1A0(%rcx)
	movups	%xmm1, 0x1B0(%rcx)
	movups	%xmm2, 0x1C0(%rcx)
	movups	%xmm3, 0x1D0(%rcx)
	movups	%xmm4, 0x1E0(%rcx)
	movups	%xmm5, 0x1F0(%rcx)
	movups	%xmm6, 0x200(%rcx)
	movups	%xmm7, 0x210(%rcx)
	movups	%xmm8, 0x220(%rcx)
	movups	%xmm9, 0x230(%rcx)
	movups	%xmm10, 0x240(%rcx)
	movups	%xmm11, 0x250(%rcx)
	movups	%xmm12, 0x260(%rcx)
	movups	%xmm13, 0x270(%rcx)
	movups	%xmm14, 0x280(%rcx)
	movups	%xmm15, 0x290(%rcx)
2:	movl	$0x10000F, 0x30(%rcx)		/* CONTEXT_AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT */
	movq	0x78(%rcx), %rax
	addq	$8, %rsp
	ret
	.seh_endproc

/* void RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec): continue at
 * ctx -- every integer register, the arithmetic flags, and (with
 * CONTEXT_FLOATING_POINT) the x87/SSE state, then Rsp and Rip at once through
 * an IRETQ (same privilege, so nothing else changes).
 * rec is not used: STATUS_LONGJUMP and STATUS_UNWIND_CONSOLIDATE records are
 * not handled. */
	.p2align 4
	.globl	rt_restore_context
	.def	rt_restore_context;	.scl	2;	.type	32;	.endef
	.seh_proc	rt_restore_context
rt_restore_context:
	.seh_endprologue
	/* W7d: with CONTEXT_FLOATING_POINT, the whole x87/SSE state -- the x87
	 * control and status words and registers, MXCSR (the context's MxCsr
	 * field, masked to its valid bits), XMM0-15 -- through FXRSTOR from an
	 * aligned copy (the context need not be aligned) */
	testl	$8, 0x30(%rcx)
	jz	2f
	subq	$0x218, %rsp
	leaq	0xF(%rsp), %rax
	andq	$-16, %rax
	xorl	%edx, %edx
1:	movups	0x100(%rcx,%rdx), %xmm0
	movaps	%xmm0, (%rax,%rdx)
	addq	$16, %rdx
	cmpq	$512, %rdx
	jb	1b
	movl	0x34(%rcx), %edx
	andl	$0xFFFF, %edx
	movl	%edx, 0x18(%rax)
	fxrstor	(%rax)
	addq	$0x218, %rsp
2:	movq	0x90(%rcx), %rbx
	movq	0xA0(%rcx), %rbp
	movq	0xA8(%rcx), %rsi
	movq	0xB0(%rcx), %rdi
	movq	0xB8(%rcx), %r8
	movq	0xC0(%rcx), %r9
	movq	0xC8(%rcx), %r10
	movq	0xD0(%rcx), %r11
	movq	0xD8(%rcx), %r12
	movq	0xE0(%rcx), %r13
	movq	0xE8(%rcx), %r14
	movq	0xF0(%rcx), %r15
	movl	%ss, %eax
	pushq	%rax				/* SS */
	pushq	0x98(%rcx)			/* RSP */
	movl	0x44(%rcx), %eax
	andl	$0xCD5, %eax			/* CF PF AF ZF SF DF OF */
	orl	$0x202, %eax
	pushq	%rax				/* RFLAGS */
	movl	%cs, %eax
	pushq	%rax				/* CS */
	pushq	0xF8(%rcx)			/* RIP */
	movq	0x78(%rcx), %rax
	movq	0x88(%rcx), %rdx
	movq	0x80(%rcx), %rcx
	iretq
	.seh_endproc

/* EXCEPTION_DISPOSITION rt_call_dispatch_handler(rec, frame, ctx, dc):
 * dc->LanguageHandler(rec, frame, ctx, dc), from a frame that keeps dc at
 * [its establisher frame + 0x20] and carries an exception handler of its own
 * (rt_dispatch_nested): an exception raised inside the handler finds this
 * frame on its way up and is told where the first dispatch had got to. */
	.p2align 4
	.globl	rt_call_dispatch_handler
	.def	rt_call_dispatch_handler;	.scl	2;	.type	32;	.endef
	.seh_proc	rt_call_dispatch_handler
rt_call_dispatch_handler:
	subq	$0x38, %rsp
	.seh_stackalloc	0x38
	.seh_endprologue
	movq	%r9, 0x20(%rsp)
	callq	*0x30(%r9)			/* DISPATCHER_CONTEXT.LanguageHandler */
	nop
	addq	$0x38, %rsp
	ret
	.seh_handler	rt_dispatch_nested, @except
	.seh_endproc

/* The same for the unwinder, with a termination handler (rt_unwind_collided):
 * an unwind started inside the handler (RtlUnwindEx from a language handler
 * during an unwind) collides with this one here and takes over from the frame
 * this one was unwinding. */
	.p2align 4
	.globl	rt_call_unwind_handler
	.def	rt_call_unwind_handler;	.scl	2;	.type	32;	.endef
	.seh_proc	rt_call_unwind_handler
rt_call_unwind_handler:
	subq	$0x38, %rsp
	.seh_stackalloc	0x38
	.seh_endprologue
	movq	%r9, 0x20(%rsp)
	callq	*0x30(%r9)
	nop
	addq	$0x38, %rsp
	ret
	.seh_handler	rt_unwind_collided, @unwind
	.seh_endproc

/* void KiUserExceptionDispatcher(void) -- W7d: where the kernel sends a
 * thread that faulted (src/win32_seh.c), with Rsp = S:
 *   S + 0x000  home space
 *   S + 0x020  CONTEXT of the fault
 *   S + 0x4F0  EXCEPTION_RECORD
 *   S + 0x590  machine frame: Rip, CS, RFLAGS, Rsp, SS of the fault
 * Never entered by a call: the "prolog" below is a description only (no
 * instruction of it is executed) -- the machine frame, the frame's size, and
 * where the fault's nonvolatile registers are (in the CONTEXT) -- so a walk
 * that reaches this frame unwinds it into the faulting function, with the
 * fault's registers, as through ntdll's on Windows. */
	.p2align 4
	.globl	rt_KiUserExceptionDispatcher
	.def	rt_KiUserExceptionDispatcher;	.scl	2;	.type	32;	.endef
	.seh_proc	rt_KiUserExceptionDispatcher
rt_KiUserExceptionDispatcher:
	.seh_pushframe
	.seh_stackalloc	0x590
	.seh_savereg	%rbx, 0xB0		/* 0x20 + CONTEXT.Rbx (0x90) */
	.seh_savereg	%rbp, 0xC0
	.seh_savereg	%rsi, 0xC8
	.seh_savereg	%rdi, 0xD0
	.seh_savereg	%r12, 0xF8
	.seh_savereg	%r13, 0x100
	.seh_savereg	%r14, 0x108
	.seh_savereg	%r15, 0x110
	.seh_savexmm	%xmm6, 0x220		/* 0x20 + CONTEXT.Xmm6 (0x200) */
	.seh_savexmm	%xmm7, 0x230
	.seh_savexmm	%xmm8, 0x240
	.seh_savexmm	%xmm9, 0x250
	.seh_savexmm	%xmm10, 0x260
	.seh_savexmm	%xmm11, 0x270
	.seh_savexmm	%xmm12, 0x280
	.seh_savexmm	%xmm13, 0x290
	.seh_savexmm	%xmm14, 0x2A0
	.seh_savexmm	%xmm15, 0x2B0
	.seh_endprologue
	leaq	0x4F0(%rsp), %rcx
	leaq	0x20(%rsp), %rdx
	call	rt_dispatch_fault
	int3
	.seh_endproc

/* memcpy and memset (not exported): what the C half's CONTEXT copies call. */
	.p2align 4
	.globl	memcpy
	.def	memcpy;	.scl	2;	.type	32;	.endef
	.seh_proc	memcpy
memcpy:
	pushq	%rdi
	.seh_pushreg	%rdi
	pushq	%rsi
	.seh_pushreg	%rsi
	.seh_endprologue
	movq	%rcx, %rax
	movq	%rcx, %rdi
	movq	%rdx, %rsi
	movq	%r8, %rcx
	rep movsb
	popq	%rsi
	popq	%rdi
	ret
	.seh_endproc

	.p2align 4
	.globl	memset
	.def	memset;	.scl	2;	.type	32;	.endef
	.seh_proc	memset
memset:
	pushq	%rdi
	.seh_pushreg	%rdi
	.seh_endprologue
	movq	%rcx, %r9
	movq	%rcx, %rdi
	movl	%edx, %eax
	movq	%r8, %rcx
	rep stosb
	movq	%r9, %rax
	popq	%rdi
	ret
	.seh_endproc
