	.globl compute
compute:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$64, %rsp
	movq	%rbx, 8(%rbp)
	movq	%r12, 16(%rbp)
	movq	%r13, 24(%rbp)
	movq	%r14, 32(%rbp)
	movq	%r15, 40(%rbp)
	# UNKNOWN: PARAM_SAVE_INT
	# UNKNOWN: PARAM_SAVE_INT
	movq	-56(%rsp),%rax
	movq	-64(%rsp),%rbx
	movq	%rax,%rax
	addq	%rbx,%rax
	movq	%rax,%rax
	movq	8(%rbp), %rbx
	movq	16(%rbp), %r12
	movq	24(%rbp), %r13
	movq	32(%rbp), %r14
	movq	40(%rbp), %r15
	leave
	ret
	.globl main
main:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$96, %rsp
	movq	%rbx, 8(%rbp)
	movq	%r12, 16(%rbp)
	movq	%r13, 24(%rbp)
	movq	%r14, 32(%rbp)
	movq	%r15, 40(%rbp)
	movq	$10,%rax
	movq	%rax,-56(%rsp)
	movq	$20,%rax
	movq	%rax,-64(%rsp)
	movq	-56(%rsp),%rax
	movq	-64(%rsp),%rbx
	movq	%rax,%rax
	addq	%rbx,%rax
	movq	%rax,-72(%rsp)
	movq	-56(%rsp),%rax
	movq	-64(%rsp),%rbx
	movq	%rax,%rax
	addq	%rbx,%rax
	movq	%rax,-80(%rsp)
	movq	-56(%rsp),%rax
	movq	-64(%rsp),%rbx
	movq	%rax,%rax
	addq	%rbx,%rax
	movq	%rax,-88(%rsp)
	movq	-72(%rsp),%rax
	movq	-80(%rsp),%rbx
	movq	%rax,%rax
	addq	%rbx,%rax
	movq	-88(%rsp),%rbx
	movq	%rax,%rax
	addq	%rbx,%rax
	movq	%rax,%rax
	movq	8(%rbp), %rbx
	movq	16(%rbp), %r12
	movq	24(%rbp), %r13
	movq	32(%rbp), %r14
	movq	40(%rbp), %r15
	leave
	ret
