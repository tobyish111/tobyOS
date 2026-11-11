/* win-flt35's faulting code -- each fault at a labelled instruction, so a
 * handler can check the address it is told and step over it. */

	.text

#define FN(name) .p2align 4; .globl name; .def name; .scl 2; .type 32; .endef; name:

FN(f_null_read)			/* read at address 0 (3 bytes) */
	xorl	%eax, %eax
	.globl	f_null_read_at
f_null_read_at:
	movq	(%rax), %rax
	ret

FN(f_ro_write)			/* rcx: a read-only page (3 bytes) */
	.globl	f_ro_write_at
f_ro_write_at:
	movq	%rax, (%rcx)
	ret

FN(f_exec)			/* rcx: a page without execute */
	jmp	*%rcx

FN(f_div0)			/* divl by a zero register (2 bytes) */
	xorl	%ecx, %ecx
	movl	$7, %eax
	xorl	%edx, %edx
	.globl	f_div0_at
f_div0_at:
	divl	%ecx
	ret

FN(f_div0_mem)			/* divq by a zero in memory (3 bytes) */
	pushq	$0
	movq	%rsp, %rcx
	movl	$7, %eax
	xorl	%edx, %edx
	.globl	f_div0_mem_at
f_div0_mem_at:
	divq	(%rcx)
	popq	%rcx
	ret

FN(f_divovf)			/* INT64_MIN / -1 (3 bytes) */
	movabsq	$0x8000000000000000, %rax
	cqo
	movq	$-1, %rcx
	.globl	f_divovf_at
f_divovf_at:
	idivq	%rcx
	ret

FN(f_ud2)			/* (2 bytes) */
	.globl	f_ud2_at
f_ud2_at:
	ud2
	ret

FN(f_hlt)			/* (1 byte) */
	.globl	f_hlt_at
f_hlt_at:
	hlt
	ret

FN(f_cli)			/* (1 byte) */
	.globl	f_cli_at
f_cli_at:
	cli
	ret

FN(f_in)			/* in al, dx (1 byte) */
	xorl	%edx, %edx
	.globl	f_in_at
f_in_at:
	inb	%dx, %al
	ret

FN(f_int3)			/* (1 byte) */
	.globl	f_int3_at
f_int3_at:
	int3
	ret

FN(f_noncanon)			/* read at a non-canonical address (3 bytes) */
	movabsq	$0x8000000000000000, %rax
	.globl	f_noncanon_at
f_noncanon_at:
	movq	(%rax), %rax
	ret

FN(f_tf)			/* TF on: the trap comes after the nop */
	pushfq
	orl	$0x100, (%rsp)
	popfq
	.globl	f_tf_at
f_tf_at:
	nop
	.globl	f_tf_next
f_tf_next:
	nop
	ret

FN(f_sse_div)			/* 1.0f / 0.0f, divide-by-zero unmasked by the caller (4 bytes) */
	movl	$1, %eax
	cvtsi2ss %eax, %xmm0
	xorps	%xmm1, %xmm1
	.globl	f_sse_div_at
f_sse_div_at:
	divss	%xmm1, %xmm0
	ret

FN(f_x87_div)			/* x87 1.0 / 0.0 with ZM unmasked: the fault at the fwait */
	subq	$8, %rsp
	fnstcw	(%rsp)
	movw	(%rsp), %ax
	andw	$0xFFFB, %ax
	movw	%ax, 2(%rsp)
	fldcw	2(%rsp)
	fld1
	fldz
	fdivrp	%st, %st(1)
	.globl	f_x87_wait_at
f_x87_wait_at:
	fwait
	fnclex
	fldcw	(%rsp)
	fstp	%st(0)
	addq	$8, %rsp
	ret

/* void f_regs(const uint64_t in[36], uint64_t out[36]): every integer
 * register but rsp from in[] (CONTEXT order: rax rcx rdx rbx rsp rbp rsi rdi
 * r8..r15 at in[0..15]), xmm6..xmm15 from in[16..35], then ud2 (2 bytes);
 * when execution goes on after it, every one of them into out[] the same
 * way. The caller's nonvolatile registers are kept. */
	.p2align 4
	.globl	f_regs
	.def	f_regs;	.scl	2;	.type	32;	.endef
	.seh_proc	f_regs
f_regs:
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
	subq	$0xB8, %rsp
	.seh_stackalloc	0xB8
	movaps	%xmm6, 0x00(%rsp)
	.seh_savexmm	%xmm6, 0x00
	movaps	%xmm7, 0x10(%rsp)
	.seh_savexmm	%xmm7, 0x10
	movaps	%xmm8, 0x20(%rsp)
	.seh_savexmm	%xmm8, 0x20
	movaps	%xmm9, 0x30(%rsp)
	.seh_savexmm	%xmm9, 0x30
	movaps	%xmm10, 0x40(%rsp)
	.seh_savexmm	%xmm10, 0x40
	movaps	%xmm11, 0x50(%rsp)
	.seh_savexmm	%xmm11, 0x50
	movaps	%xmm12, 0x60(%rsp)
	.seh_savexmm	%xmm12, 0x60
	movaps	%xmm13, 0x70(%rsp)
	.seh_savexmm	%xmm13, 0x70
	movaps	%xmm14, 0x80(%rsp)
	.seh_savexmm	%xmm14, 0x80
	movaps	%xmm15, 0x90(%rsp)
	.seh_savexmm	%xmm15, 0x90
	.seh_endprologue
	movq	%rdx, 0xA0(%rsp)
	movups	0x80(%rcx), %xmm6
	movups	0x90(%rcx), %xmm7
	movups	0xA0(%rcx), %xmm8
	movups	0xB0(%rcx), %xmm9
	movups	0xC0(%rcx), %xmm10
	movups	0xD0(%rcx), %xmm11
	movups	0xE0(%rcx), %xmm12
	movups	0xF0(%rcx), %xmm13
	movups	0x100(%rcx), %xmm14
	movups	0x110(%rcx), %xmm15
	movq	0x00(%rcx), %rax
	movq	0x10(%rcx), %rdx
	movq	0x18(%rcx), %rbx
	movq	0x28(%rcx), %rbp
	movq	0x30(%rcx), %rsi
	movq	0x38(%rcx), %rdi
	movq	0x40(%rcx), %r8
	movq	0x48(%rcx), %r9
	movq	0x50(%rcx), %r10
	movq	0x58(%rcx), %r11
	movq	0x60(%rcx), %r12
	movq	0x68(%rcx), %r13
	movq	0x70(%rcx), %r14
	movq	0x78(%rcx), %r15
	movq	0x08(%rcx), %rcx
	.globl	f_regs_at
f_regs_at:
	ud2
	movq	%rax, 0xA8(%rsp)
	movq	0xA0(%rsp), %rax
	movq	%rcx, 0x08(%rax)
	movq	%rdx, 0x10(%rax)
	movq	%rbx, 0x18(%rax)
	movq	%rbp, 0x28(%rax)
	movq	%rsi, 0x30(%rax)
	movq	%rdi, 0x38(%rax)
	movq	%r8, 0x40(%rax)
	movq	%r9, 0x48(%rax)
	movq	%r10, 0x50(%rax)
	movq	%r11, 0x58(%rax)
	movq	%r12, 0x60(%rax)
	movq	%r13, 0x68(%rax)
	movq	%r14, 0x70(%rax)
	movq	%r15, 0x78(%rax)
	movq	0xA8(%rsp), %rcx
	movq	%rcx, 0x00(%rax)
	movups	%xmm6, 0x80(%rax)
	movups	%xmm7, 0x90(%rax)
	movups	%xmm8, 0xA0(%rax)
	movups	%xmm9, 0xB0(%rax)
	movups	%xmm10, 0xC0(%rax)
	movups	%xmm11, 0xD0(%rax)
	movups	%xmm12, 0xE0(%rax)
	movups	%xmm13, 0xF0(%rax)
	movups	%xmm14, 0x100(%rax)
	movups	%xmm15, 0x110(%rax)
	movaps	0x00(%rsp), %xmm6
	movaps	0x10(%rsp), %xmm7
	movaps	0x20(%rsp), %xmm8
	movaps	0x30(%rsp), %xmm9
	movaps	0x40(%rsp), %xmm10
	movaps	0x50(%rsp), %xmm11
	movaps	0x60(%rsp), %xmm12
	movaps	0x70(%rsp), %xmm13
	movaps	0x80(%rsp), %xmm14
	movaps	0x90(%rsp), %xmm15
	addq	$0xB8, %rsp
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

/* uint64_t f_deep(void): rbx is the caller's; it is pushed, clobbered, and
 * then the read at 0 faults -- a frame to unwind out of. */
	.p2align 4
	.globl	f_deep
	.def	f_deep;	.scl	2;	.type	32;	.endef
	.seh_proc	f_deep
f_deep:
	pushq	%rbx
	.seh_pushreg	%rbx
	subq	$0x20, %rsp
	.seh_stackalloc	0x20
	.seh_endprologue
	movq	$0xDEAD, %rbx
	xorl	%eax, %eax
	.globl	f_deep_at
f_deep_at:
	movq	(%rax), %rax
	nop
	addq	$0x20, %rsp
	popq	%rbx
	ret
	.seh_endproc

/* uint64_t f_scoped(uint64_t *rbx_at_except):
 *     __try {                       -- scope 1: __except (s_filter)
 *         __try { f_deep(); }       -- scope 0: __finally (s_finally)
 *         __finally { s_finally }
 *     } __except (s_filter) { *rbx_at_except = rbx; return the code; }
 *     return 0;
 * rbx holds a pattern across the call, so the __except block shows whether
 * the unwind out of f_deep restored it. */
	.p2align 4
	.globl	f_scoped
	.def	f_scoped;	.scl	2;	.type	32;	.endef
	.seh_proc	f_scoped
f_scoped:
	pushq	%rbx
	.seh_pushreg	%rbx
	pushq	%rsi
	.seh_pushreg	%rsi
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
	movq	%rcx, %rsi
	movabsq	$0x5C0FED0B5C0FED, %rbx
.Ls_try_begin:
	call	f_deep
	nop
.Ls_try_end:
	nop
	xorl	%eax, %eax
	jmp	.Ls_out
.Ls_except:
	movq	%rbx, (%rsi)
.Ls_out:
	addq	$0x28, %rsp
	popq	%rsi
	popq	%rbx
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	2
	.rva	.Ls_try_begin
	.rva	.Ls_try_end
	.rva	s_finally
	.long	0
	.rva	.Ls_try_begin
	.rva	.Ls_try_end
	.rva	s_filter
	.rva	.Ls_except
	.text
	.seh_endproc

/* void f_fin(void): __try { read at 0 } __finally { s_finally } -- nothing
 * takes the fault; the unwind of an unhandled exception runs the __finally. */
	.p2align 4
	.globl	f_fin
	.def	f_fin;	.scl	2;	.type	32;	.endef
	.seh_proc	f_fin
f_fin:
	subq	$0x28, %rsp
	.seh_stackalloc	0x28
	.seh_endprologue
	xorl	%eax, %eax
.Lf_try_begin:
	movq	(%rax), %rax
	nop
.Lf_try_end:
	nop
	addq	$0x28, %rsp
	ret
	.seh_handler	__C_specific_handler, @except, @unwind
	.seh_handlerdata
	.long	1
	.rva	.Lf_try_begin
	.rva	.Lf_try_end
	.rva	s_finally
	.long	0
	.text
	.seh_endproc
