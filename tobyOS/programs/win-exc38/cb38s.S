/* win-cb38's frames.
 *
 * uint64_t f_try(uint64_t (*fn)(void *), void *arg):
 *     __try { __try { r = fn(arg); } __finally { cb_finally } }
 *     __except (cb_filter) { return the exception code; }
 *     return r | (1ull << 62);
 *   -- bit 62 tells a value from a code.
 *
 * uint64_t f_fin(uint64_t (*fn)(void *), void *arg):
 *     __try { return fn(arg); } __finally { cb_finally } */
	.text
	.p2align 4
	.globl	f_try
	.def	f_try;	.scl	2;	.type	32;	.endef
	.seh_proc	f_try
f_try:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
	movq	%rcx, %rax
	movq	%rdx, %rcx
.Lt_b:
	call	*%rax
	nop
.Lt_e:
	nop
	movabsq	$0x4000000000000000, %rdx
	orq	%rdx, %rax
.Lt_x:
	addq	$0x28, %rsp
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	2
	.rva	.Lt_b
	.rva	.Lt_e
	.rva	cb_finally
	.long	0
	.rva	.Lt_b
	.rva	.Lt_e
	.rva	cb_filter
	.rva	.Lt_x
	.text
	.seh_endproc

	.p2align 4
	.globl	f_fin
	.def	f_fin;	.scl	2;	.type	32;	.endef
	.seh_proc	f_fin
f_fin:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
	movq	%rcx, %rax
	movq	%rdx, %rcx
.Lf_b:
	call	*%rax
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
	.rva	cb_finally
	.long	0
	.text
	.seh_endproc
