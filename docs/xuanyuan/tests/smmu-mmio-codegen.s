	.file	"test-smmu-mmio.c"
	.text
	.globl	zero                            // -- Begin function zero
	.p2align	2
	.type	zero,@function
zero:                                   // @zero
// %bb.0:
	mov	w8, wzr
	//APP
	str	w8, [x0]
	//NO_APP
	ret
.Lfunc_end0:
	.size	zero, .Lfunc_end0-zero
                                        // -- End function
	.globl	all_ones                        // -- Begin function all_ones
	.p2align	2
	.type	all_ones,@function
all_ones:                               // @all_ones
// %bb.0:
	mov	w8, #-1                         // =0xffffffff
	//APP
	str	w8, [x0]
	//NO_APP
	ret
.Lfunc_end1:
	.size	all_ones, .Lfunc_end1-all_ones
                                        // -- End function
	.globl	variable                        // -- Begin function variable
	.p2align	2
	.type	variable,@function
variable:                               // @variable
// %bb.0:
	//APP
	str	w1, [x0]
	//NO_APP
	ret
.Lfunc_end2:
	.size	variable, .Lfunc_end2-variable
                                        // -- End function
	.globl	baseline_zero                   // -- Begin function baseline_zero
	.p2align	2
	.type	baseline_zero,@function
baseline_zero:                          // @baseline_zero
// %bb.0:
	str	wzr, [x0]
	ret
.Lfunc_end3:
	.size	baseline_zero, .Lfunc_end3-baseline_zero
                                        // -- End function
	.ident	"Ubuntu clang version 21.1.8 (++20251221032922+2078da43e25a-1~exp1~20251221153059.70)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
