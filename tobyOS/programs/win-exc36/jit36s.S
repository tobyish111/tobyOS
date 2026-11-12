/* win-jit36's frame: uint64_t f_try_rbx(uint64_t (*fn)(void *), void *arg,
 * uint64_t *rbx_out):
 *     rbx = pattern; __try { fn(arg); } __except (jit_filter) {
 *         *rbx_out = rbx; return the code; } return 0;
 * -- so the __except block shows whether the unwind out of the generated
 * frame (which saves rbx and clobbers it) restored rbx. */
	.text
	.p2align 4
	.globl	f_try_rbx
	.def	f_try_rbx;	.scl	2;	.type	32;	.endef
	.seh_proc	f_try_rbx
f_try_rbx:
	pushq	%rbx
	.seh_pushreg	%rbx
	pushq	%rsi
	.seh_pushreg	%rsi
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
	movq	%r8, %rsi
	movabsq	$0x5C0FED0B5C0FED, %rbx
	movq	%rcx, %rax
	movq	%rdx, %rcx
.Lb:
	call	*%rax
	nop
.Le:
	nop
	xorl	%eax, %eax
	jmp	.Lo
.Lx:
	movq	%rbx, (%rsi)
.Lo:
	addq	$0x28, %rsp
	popq	%rsi
	popq	%rbx
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	1
	.rva	.Lb
	.rva	.Le
	.rva	jit_filter
	.rva	.Lx
	.text
	.seh_endproc
