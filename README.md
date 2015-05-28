# Optimized Universal Machine
Alexander Ravan and Max Bennett
November 20, 2014

This is an optimized version of the universal machine located in the universal machine repository. 

## Assembly Routine
Our most expensive and time consuming procedure is mapping a segment. 
Half of this routine is devoted to calloc, approximately a third is devoted to freeing unmapped segments, and the remaining fifth of the routine is devoted to removing unmapped segment IDs from our sequence.

We have analyzed the assembly code and do not see any opportunities to further optimize. With more time we would change our implementation of the unmapped segment IDs to an array, as Hanson's Sequences are are quite slow. 

Although this routine is our most expensive procedure, most of the expense comes from functions that we do not have control over, nor know how to implement, so we do not believe there is much more for us to optimize.