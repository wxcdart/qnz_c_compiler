	.globl add
add:
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
	.globl subtract
subtract:
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
	subq	%rbx,%rax
	movq	%rax,%rax
	movq	8(%rbp), %rbx
	movq	16(%rbp), %r12
	movq	24(%rbp), %r13
	movq	32(%rbp), %r14
	movq	40(%rbp), %r15
	leave
	ret
	.globl multiply
multiply:
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
	imulq	%rbx
	movq	%rax,%rax
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
	subq	$112, %rsp
	movq	%rbx, 8(%rbp)
	movq	%r12, 16(%rbp)
	movq	%r13, 24(%rbp)
	movq	%r14, 32(%rbp)
	movq	%r15, 40(%rbp)
	movq	$0,%rax
	movq	%rax,-56(%rsp)
	movq	$10,%rax
	movq	%rax,-64(%rsp)
	movq	$5,%rax
	movq	%rax,-72(%rsp)
	movq	-64(%rsp),%rax
	movq	-72(%rsp),%rbx
	movq	%rax, rdi
	movq	%rax, rsi
	call	add
	movq	%rax,%rax
	movq	%rax,-56(%rsp)
	movq	-56(%rsp),%rax
	movq	$15,%rbx
	cmpq	%rbx,%rax
	setne	%al
	movzbl	%al,%rax
	movq	$1,%rax
	movq	%rax,%rax
	movq	$10,%rbx
	movq	%rbx,-80(%rsp)
	movq	$5,%rbx
	movq	%rbx,-88(%rsp)
	movq	-80(%rsp),%rbx
	movq	-88(%rsp),%rcx
	movq	%rax, rdi
	movq	%rax, rsi
	call	subtract
	movq	%rax,%rbx
	movq	%rbx,-56(%rsp)
	movq	-56(%rsp),%rbx
	movq	$5,%rcx
	cmpq	%rcx,%rbx
	setne	%al
	movzbl	%al,%rbx
	movq	$2,%rbx
	movq	%rbx,%rax
	movq	$10,%rbx
	movq	%rbx,-96(%rsp)
	movq	$5,%rbx
	movq	%rbx,-104(%rsp)
	movq	-96(%rsp),%rbx
	movq	-104(%rsp),%rcx
	movq	%rax, rdi
	movq	%rax, rsi
	call	multiply
	movq	%rax,%rbx
	movq	%rbx,-56(%rsp)
	movq	-56(%rsp),%rbx
	movq	$50,%rcx
	cmpq	%rcx,%rbx
	setne	%al
	movzbl	%al,%rbx
	movq	$3,%rbx
	movq	%rbx,%rax
	movq	$0,%rbx
	movq	%rbx,%rax
	movq	8(%rbp), %rbx
	movq	16(%rbp), %r12
	movq	24(%rbp), %r13
	movq	32(%rbp), %r14
	movq	40(%rbp), %r15
	leave
	ret
