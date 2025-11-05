/*
 * switch.S - assembly routines for switching trap frames
 */

/*
 * Trap Frame Format
 * WARNING: These values reflect the layout of struct thread_tf. Don't change
 * these values without also updating defs.h.
 */

.file "switch_simdreg_custom.S"
.section        .note.GNU-stack,"",@progbits
.text

/* arguments registers (can be clobbered) */
#define RDI	(0)
#define RSI	(8)
#define RDX	(16)
#define RCX	(24)
#define R8	(32)
#define R9	(40)

/* temporary registers (can be clobbered) */
#define R10	(48)
#define R11	(56)

/* callee-saved registers (can not be clobbered) */
#define RBX	(64)
#define RBP	(72)
#define R12	(80)
#define R13	(88)
#define R14	(96)
#define R15	(104)

/* special-purpose registers */
#define RAX	(112)	/* return code */
#define RIP	(120)	/* instruction pointer */
#define RSP	(128)	/* stack pointer */

/* mask registers */
#define K1  (136)
#define K2  (144)
#define K3  (152)
#define K4  (160)
#define K5  (168)
#define K6  (176)
#define K7  (184)

/* AVX registers */
#define YMM0 (192)
#define YMM1 (224)
#define YMM16 (256)
#define YMM17 (288)
#define YMM18 (320)
#define YMM19 (352)
#define YMM20 (384)
/**
 * __jmp_thread - executes a thread from the runtime
 * @tf: the trap frame to restore (%rdi)
 *
 * This low-level variant isn't intended to be called directly.
 * Re-enables preemption, parking the kthread if necessary.
 * Does not return.
 */
.align 16
.globl __jmp_thread
.type __jmp_thread, @function
__jmp_thread:
	/* restore ip and stack */
	movq    RSP(%rdi), %rsp
	movq    RIP(%rdi), %rsi

	/* restore callee regs */
	movq    RBX(%rdi), %rbx
	movq    RBP(%rdi), %rbp
	movq    R12(%rdi), %r12
	movq    R13(%rdi), %r13
	movq    R14(%rdi), %r14
	movq    R15(%rdi), %r15

	/* kmovq   K0(%rdi), %k0 */
	kmovq   K1(%rdi), %k1
	kmovq   K2(%rdi), %k2
	kmovq   K3(%rdi), %k3
	kmovq   K4(%rdi), %k4
	kmovq   K5(%rdi), %k5
	kmovq   K6(%rdi), %k6
	kmovq   K7(%rdi), %k7

	VMOVDQA64 YMM0(%rdi),  %ymm0
	VMOVDQA64 YMM1(%rdi),  %ymm1
	VMOVDQA64 YMM16(%rdi), %ymm16
	VMOVDQA64 YMM17(%rdi), %ymm17
	VMOVDQA64 YMM18(%rdi), %ymm18
	VMOVDQA64 YMM19(%rdi), %ymm19
	VMOVDQA64 YMM20(%rdi), %ymm20
	
	/* set first argument (in case new thread) */
	movq    RDI(%rdi), %rdi /* ARG0 */

	/* re-enable preemption */
	subl	$1, %gs:__perthread_preempt_cnt(%rip)
	jz	1f

	/* jump into trap frame */
	STUI
	jmpq	*%rsi
	nop

1:	/* cold-path, save RIP and park the kthread */
	pushq	%rsi
	pushq	%rdi
	pushq	%r15
	movq	%rsp, %r15
	andq	$0xfffffffffffffff0, %rsp /* ensure correct stack alignment */
	call	preempt
	movq	%r15, %rsp /* restore SP */
	popq	%r15
	popq	%rdi
	popq	%rsi
	STUI
	jmpq	*%rsi

/**
 * __jmp_thread_direct - directly switches from one thread to the next
 * @oldtf: the trap frame to save (%rdi)
 * @newtf: the trap frame to restore (%rsi)
 * @thread_running: a pointer to whether the thread is still running (%rdx)
 *
 * This low-level variant isn't intended to be called directly.
 * Re-enables preemption, parking the kthread if necessary.
 * Does return.
 */
.align 16
.globl __jmp_thread_direct
.type __jmp_thread_direct, @function
__jmp_thread_direct:
	/* save ip and stack */
	movq    (%rsp), %r8
	movq    %r8, RIP(%rdi)
	leaq    8(%rsp), %r8
	movq    %r8, RSP(%rdi)

	/* save callee regs */
	movq    %rbx, RBX(%rdi)
	movq    %rbp, RBP(%rdi)
	movq    %r12, R12(%rdi)
	movq    %r13, R13(%rdi)
	movq    %r14, R14(%rdi)
	movq    %r15, R15(%rdi)

	/* kmovq   %k0, K0(%rdi) */
	kmovq   %k1, K1(%rdi)
	kmovq   %k2, K2(%rdi)
	kmovq   %k3, K3(%rdi)
	kmovq   %k4, K4(%rdi)
	kmovq   %k5, K5(%rdi)
	kmovq   %k6, K6(%rdi)
	kmovq   %k7, K7(%rdi)

	/* VMOVDQA64 %ymm0,  YMM0(%rdi) */
	/*movdqa %xmm1,  YMM1(%rdi) */
	VMOVDQA64 %ymm0,  YMM0(%rdi)  
	VMOVDQA64 %ymm1,  YMM1(%rdi)
	VMOVDQA64 %ymm16, YMM16(%rdi) 
	VMOVDQA64 %ymm17, YMM17(%rdi) 
	VMOVDQA64 %ymm18, YMM18(%rdi) 
	VMOVDQA64 %ymm19, YMM19(%rdi) 
	VMOVDQA64 %ymm20, YMM20(%rdi)

	/* restore ip and stack */
	movq    RSP(%rsi), %rsp
	movq    RIP(%rsi), %r8

	/* clear the stack busy flag */
	movl	$0, (%rdx)

	/* restore callee regs */
	movq    RBX(%rsi), %rbx
	movq    RBP(%rsi), %rbp
	movq    R12(%rsi), %r12
	movq    R13(%rsi), %r13
	movq    R14(%rsi), %r14
	movq    R15(%rsi), %r15

	/* kmovq   K0(%rsi), %k0 */
	kmovq   K1(%rsi), %k1
	kmovq   K2(%rsi), %k2
	kmovq   K3(%rsi), %k3
	kmovq   K4(%rsi), %k4
	kmovq   K5(%rsi), %k5
	kmovq   K6(%rsi), %k6
	kmovq   K7(%rsi), %k7

	/* VMOVDQA64 YMM0(%rsi),  %ymm0 */
	/* movdqa YMM1(%rsi),  %xmm1 */ 
	VMOVDQA64 YMM0(%rsi),  %ymm0
	VMOVDQA64 YMM1(%rsi),  %ymm1
	VMOVDQA64 YMM16(%rsi), %ymm16
	VMOVDQA64 YMM17(%rsi), %ymm17
	VMOVDQA64 YMM18(%rsi), %ymm18
	VMOVDQA64 YMM19(%rsi), %ymm19
	VMOVDQA64 YMM20(%rsi), %ymm20

	/* set first argument (in case new thread) */
	movq    RDI(%rsi), %rdi /* ARG0 */

	/* re-enable preemption */
	subl	$1, %gs:__perthread_preempt_cnt(%rip)
	jz	1f

	/* jump into trap frame */
	STUI
	jmpq	*%r8
	nop

1:	/* cold-path, save RIP and park the kthread */
	pushq	%r8
	pushq	%rdi
	pushq	%r15
	movq	%rsp, %r15
	andq	$0xfffffffffffffff0, %rsp /* ensure correct stack alignment */
	call	preempt
	movq	%r15, %rsp /* restore SP */
	popq	%r15
	popq	%rdi
	popq	%r8
	STUI
	jmpq	*%r8

/**
 * __jmp_runtime - saves the current trap frame and jumps to a function in the
 *                 runtime
 * @tf: the struct thread_tf to save state (%rdi)
 * @fn: the function pointer to call (%rsi)
 * @stack: the start of the runtime stack (%rdx)
 *
 * This low-level variant isn't intended to be called directly.
 * Must be called with preemption disabled.
 * No return value.
 */
.align 16
.globl __jmp_runtime
.type __jmp_runtime, @function
__jmp_runtime:
	/* save callee regs */
	movq    %rbx, RBX(%rdi)
	movq    %rbp, RBP(%rdi)
	movq    %r12, R12(%rdi)
	movq    %r13, R13(%rdi)
	movq    %r14, R14(%rdi)
	movq    %r15, R15(%rdi)

	/* kmovq   %k0, K0(%rdi) */
	kmovq   %k1, K1(%rdi)
	kmovq   %k2, K2(%rdi)
	kmovq   %k3, K3(%rdi)
	kmovq   %k4, K4(%rdi)
	kmovq   %k5, K5(%rdi)
	kmovq   %k6, K6(%rdi)
	kmovq   %k7, K7(%rdi)

	VMOVDQA64 %ymm0,  YMM0(%rdi)  
	VMOVDQA64 %ymm1,  YMM1(%rdi)
	VMOVDQA64 %ymm16, YMM16(%rdi) 
	VMOVDQA64 %ymm17, YMM17(%rdi) 
	VMOVDQA64 %ymm18, YMM18(%rdi) 
	VMOVDQA64 %ymm19, YMM19(%rdi) 
	VMOVDQA64 %ymm20, YMM20(%rdi)
	
	/* save ip and stack */
	movq    (%rsp), %r8
	movq    %r8, RIP(%rdi)
	leaq    8(%rsp), %r8
	movq    %r8, RSP(%rdi)

	/* jump into runtime function */
	movq    %rdx, %rsp

	/* jump into runtime code */
	jmpq    *%rsi

/**
 * __jmp_runtime_nosave - jumps to a function in the runtime without saving the
 *			  current stack frame
 * @fn: the function pointer to call (%rdi)
 * @stack: the start of the runtime stack (%rsi)
 *
 * This low-level variant isn't intended to be called directly.
 * Must be called with preemption disabled.
 * No return value.
 */
.align 16
.globl __jmp_runtime_nosave
.type __jmp_runtime_nosave, @function
__jmp_runtime_nosave:

	/* jump into runtime function */
	movq    %rsi, %rsp
	movq	%rdi, %rsi

	/* jump into runtime code */
	jmpq    *%rsi