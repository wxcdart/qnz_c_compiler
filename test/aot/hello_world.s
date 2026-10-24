	.globl main
main:
	subq	$72, %rsp
	leaq	.Lrodata+0(%rip),%rdi
	movq	%rdi,8(%rsp)
	movq	%rdi,%rsi
	pushq	%rsi
	popq	%rdi
	call	printf
	movq	$0,%rdi
	movslq	%edi,%rdi
	movq	%rdi,%rax
.L19_0:
	addq	$72, %rsp
	ret
	.section	.rodata
	.p2align	3
.Lrodata:
	.byte	0x48
	.byte	0x65
	.byte	0x6c
	.byte	0x6c
	.byte	0x6f
	.byte	0x2c
	.byte	0x20
	.byte	0x57
	.byte	0x6f
	.byte	0x72
	.byte	0x6c
	.byte	0x64
	.byte	0x21
	.byte	0x0a
	.byte	0x00
	.text
