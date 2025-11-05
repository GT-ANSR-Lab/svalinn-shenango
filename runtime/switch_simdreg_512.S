/*
 * switch.S - assembly routines for switching trap frames
 */

/*
 * Trap Frame Format
 * WARNING: These values reflect the layout of struct thread_tf. Don't change
 * these values without also updating defs.h.
 */

.file "switch_simdreg_512.S"
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
#define ZMM0 (192)
#define ZMM1 (256)
#define ZMM2 (320)
#define ZMM3 (384)
#define ZMM4 (448)
#define ZMM5 (512)
#define ZMM6 (576)
#define ZMM7 (640)
#define ZMM8 (704)
#define ZMM9 (768)
#define ZMM10 (832)
#define ZMM11 (896)
#define ZMM12 (960)
#define ZMM13 (1024)
#define ZMM14 (1088)
#define ZMM15 (1152)
#define ZMM16 (1216)
#define ZMM17 (1280)
#define ZMM18 (1344)
#define ZMM19 (1408)
#define ZMM20 (1472)
#define ZMM21 (1536)
#define ZMM22 (1600)
#define ZMM23 (1664)
#define ZMM24 (1728)
#define ZMM25 (1792)
#define ZMM26 (1856)
#define ZMM27 (1920)
#define ZMM28 (1984)
#define ZMM29 (2048)
#define ZMM30 (2112)
#define ZMM31 (2176)

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

	VMOVDQA64 ZMM0(%rdi),  %zmm0
	VMOVDQA64 ZMM1(%rdi),  %zmm1
	VMOVDQA64 ZMM2(%rdi),  %zmm2
	VMOVDQA64 ZMM3(%rdi),  %zmm3
	VMOVDQA64 ZMM4(%rdi),  %zmm4
	VMOVDQA64 ZMM5(%rdi),  %zmm5
	VMOVDQA64 ZMM6(%rdi),  %zmm6
	VMOVDQA64 ZMM7(%rdi),  %zmm7
	VMOVDQA64 ZMM8(%rdi),  %zmm8
	VMOVDQA64 ZMM9(%rdi),  %zmm9
	VMOVDQA64 ZMM10(%rdi), %zmm10
	VMOVDQA64 ZMM11(%rdi), %zmm11
	VMOVDQA64 ZMM12(%rdi), %zmm12
	VMOVDQA64 ZMM13(%rdi), %zmm13
	VMOVDQA64 ZMM14(%rdi), %zmm14
	VMOVDQA64 ZMM15(%rdi), %zmm15
	
	VMOVDQA64 ZMM16(%rdi), %zmm16
	VMOVDQA64 ZMM17(%rdi), %zmm17
	VMOVDQA64 ZMM18(%rdi), %zmm18
	VMOVDQA64 ZMM19(%rdi), %zmm19
	VMOVDQA64 ZMM20(%rdi), %zmm20
	VMOVDQA64 ZMM21(%rdi), %zmm21
	VMOVDQA64 ZMM22(%rdi), %zmm22
	VMOVDQA64 ZMM23(%rdi), %zmm23
	VMOVDQA64 ZMM24(%rdi), %zmm24
	VMOVDQA64 ZMM25(%rdi), %zmm25
	VMOVDQA64 ZMM26(%rdi), %zmm26
	VMOVDQA64 ZMM27(%rdi), %zmm27
	VMOVDQA64 ZMM28(%rdi), %zmm28
	VMOVDQA64 ZMM29(%rdi), %zmm29
	VMOVDQA64 ZMM30(%rdi), %zmm30
	VMOVDQA64 ZMM31(%rdi), %zmm31

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

	VMOVDQA64 %zmm0,  ZMM0(%rdi)  
	VMOVDQA64 %zmm1,  ZMM1(%rdi)
	VMOVDQA64 %zmm2,  ZMM2(%rdi)
	VMOVDQA64 %zmm3,  ZMM3(%rdi)
	VMOVDQA64 %zmm4,  ZMM4(%rdi)
	VMOVDQA64 %zmm5,  ZMM5(%rdi)
	VMOVDQA64 %zmm6,  ZMM6(%rdi)
	VMOVDQA64 %zmm7,  ZMM7(%rdi)
	VMOVDQA64 %zmm8,  ZMM8(%rdi)
	VMOVDQA64 %zmm9,  ZMM9(%rdi)
	VMOVDQA64 %zmm10, ZMM10(%rdi)
	VMOVDQA64 %zmm11, ZMM11(%rdi)
	VMOVDQA64 %zmm12, ZMM12(%rdi)
	VMOVDQA64 %zmm13, ZMM13(%rdi)
	VMOVDQA64 %zmm14, ZMM14(%rdi)
	VMOVDQA64 %zmm15, ZMM15(%rdi)

	VMOVDQA64 %zmm16, ZMM16(%rdi) 
	VMOVDQA64 %zmm17, ZMM17(%rdi) 
	VMOVDQA64 %zmm18, ZMM18(%rdi) 
	VMOVDQA64 %zmm19, ZMM19(%rdi) 
	VMOVDQA64 %zmm20, ZMM20(%rdi)
	VMOVDQA64 %zmm21, ZMM21(%rdi)
	VMOVDQA64 %zmm22, ZMM22(%rdi)
	VMOVDQA64 %zmm23, ZMM23(%rdi)
	VMOVDQA64 %zmm24, ZMM24(%rdi)
	VMOVDQA64 %zmm25, ZMM25(%rdi)
	VMOVDQA64 %zmm26, ZMM26(%rdi)
	VMOVDQA64 %zmm27, ZMM27(%rdi)
	VMOVDQA64 %zmm28, ZMM28(%rdi)
	VMOVDQA64 %zmm29, ZMM29(%rdi)
	VMOVDQA64 %zmm30, ZMM30(%rdi)
	VMOVDQA64 %zmm31, ZMM31(%rdi)

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

	VMOVDQA64 ZMM0(%rsi),  %zmm0
	VMOVDQA64 ZMM1(%rsi),  %zmm1
	VMOVDQA64 ZMM2(%rsi),  %zmm2
	VMOVDQA64 ZMM3(%rsi),  %zmm3
	VMOVDQA64 ZMM4(%rsi),  %zmm4
	VMOVDQA64 ZMM5(%rsi),  %zmm5
	VMOVDQA64 ZMM6(%rsi),  %zmm6
	VMOVDQA64 ZMM7(%rsi),  %zmm7
	VMOVDQA64 ZMM8(%rsi),  %zmm8
	VMOVDQA64 ZMM9(%rsi),  %zmm9
	VMOVDQA64 ZMM10(%rsi), %zmm10
	VMOVDQA64 ZMM11(%rsi), %zmm11
	VMOVDQA64 ZMM12(%rsi), %zmm12
	VMOVDQA64 ZMM13(%rsi), %zmm13
	VMOVDQA64 ZMM14(%rsi), %zmm14
	VMOVDQA64 ZMM15(%rsi), %zmm15

	VMOVDQA64 ZMM16(%rsi), %zmm16
	VMOVDQA64 ZMM17(%rsi), %zmm17
	VMOVDQA64 ZMM18(%rsi), %zmm18
	VMOVDQA64 ZMM19(%rsi), %zmm19
	VMOVDQA64 ZMM20(%rsi), %zmm20
	VMOVDQA64 ZMM21(%rsi), %zmm21
	VMOVDQA64 ZMM22(%rsi), %zmm22
	VMOVDQA64 ZMM23(%rsi), %zmm23
	VMOVDQA64 ZMM24(%rsi), %zmm24
	VMOVDQA64 ZMM25(%rsi), %zmm25
	VMOVDQA64 ZMM26(%rsi), %zmm26
	VMOVDQA64 ZMM27(%rsi), %zmm27
	VMOVDQA64 ZMM28(%rsi), %zmm28
	VMOVDQA64 ZMM29(%rsi), %zmm29
	VMOVDQA64 ZMM30(%rsi), %zmm30
	VMOVDQA64 ZMM31(%rsi), %zmm31

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

	VMOVDQA64 %zmm0,  ZMM0(%rdi)  
	VMOVDQA64 %zmm1,  ZMM1(%rdi)
	VMOVDQA64 %zmm2,  ZMM2(%rdi)
	VMOVDQA64 %zmm3,  ZMM3(%rdi)
	VMOVDQA64 %zmm4,  ZMM4(%rdi)
	VMOVDQA64 %zmm5,  ZMM5(%rdi)
	VMOVDQA64 %zmm6,  ZMM6(%rdi)
	VMOVDQA64 %zmm7,  ZMM7(%rdi)
	VMOVDQA64 %zmm8,  ZMM8(%rdi)
	VMOVDQA64 %zmm9,  ZMM9(%rdi)
	VMOVDQA64 %zmm10, ZMM10(%rdi)
	VMOVDQA64 %zmm11, ZMM11(%rdi)
	VMOVDQA64 %zmm12, ZMM12(%rdi)
	VMOVDQA64 %zmm13, ZMM13(%rdi)
	VMOVDQA64 %zmm14, ZMM14(%rdi)
	VMOVDQA64 %zmm15, ZMM15(%rdi)

	VMOVDQA64 %zmm16, ZMM16(%rdi) 
	VMOVDQA64 %zmm17, ZMM17(%rdi) 
	VMOVDQA64 %zmm18, ZMM18(%rdi) 
	VMOVDQA64 %zmm19, ZMM19(%rdi) 
	VMOVDQA64 %zmm20, ZMM20(%rdi)
	VMOVDQA64 %zmm21, ZMM21(%rdi)
	VMOVDQA64 %zmm22, ZMM22(%rdi)
	VMOVDQA64 %zmm23, ZMM23(%rdi)
	VMOVDQA64 %zmm24, ZMM24(%rdi)
	VMOVDQA64 %zmm25, ZMM25(%rdi)
	VMOVDQA64 %zmm26, ZMM26(%rdi)
	VMOVDQA64 %zmm27, ZMM27(%rdi)
	VMOVDQA64 %zmm28, ZMM28(%rdi)
	VMOVDQA64 %zmm29, ZMM29(%rdi)
	VMOVDQA64 %zmm30, ZMM30(%rdi)
	VMOVDQA64 %zmm31, ZMM31(%rdi)

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