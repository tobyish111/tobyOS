/* win-so35's frame: uint64_t f_try(uint64_t (*fn)(void)):
 *     __try { return fn(); } __except (so_filter) { return the code; }
 * (__C_specific_handler; the filter records the exception). */
	.text
	.p2align 4
	.globl	f_try
	.def	f_try;	.scl	2;	.type	32;	.endef
	.seh_proc	f_try
f_try:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
.Lb:
	call	*%rcx
	nop
.Le:
	nop
	jmp	.Lo
.Lx:
	nop
.Lo:
	addq	$0x28, %rsp
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	1
	.rva	.Lb
	.rva	.Le
	.rva	so_filter
	.rva	.Lx
	.text
	.seh_endproc
