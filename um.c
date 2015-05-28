/*
 * um.c
 * Homework 7 -- Profiling 
 * 
 * Name: Alexander Ravan(aravan01)
 *       Maxwell Bennet(mbenne06)
 *
 * Date: Thursday, November 20th, 2014
 *
 * Summary: This is the optimized version of our Universal Machine Program,
 *          together with the segmented memory module, as well as the bitpacking
 *          module. 
*/  

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include "stdint.h"
#include "assert.h"
#include "mem.h"
#include "seq.h"
#include "um-opcode.h"
#include "stdbool.h"
#include "except.h"

typedef uint32_t UMsegment_ID;
typedef uint32_t UMword_ID;
typedef uint32_t UMword;
typedef UMword *Segment;
typedef Segment *SegArray;

typedef struct segmentedMem {
        SegArray segmentArr;
        int numSegments;
        int arraySize;
        Seq_T unmappedSegs;
} *segmentedMem;

/**************************** Bitpack Module *********************************/
/*************** Taken from the Arith (HW6) Solution bitpack.c ***************/

Except_T Bitpack_Overflow = { "Overflow packing bits" };
static inline uint64_t shl(uint64_t word, unsigned bits)
{
        assert(bits <= 64);
        if (bits == 64)
                return 0;
        else
                return word << bits;
}
static inline uint64_t shr(uint64_t word, unsigned bits)
{
        assert(bits <= 64);
        if (bits == 64)
                return 0;
        else
                return word >> bits;
}
static inline bool Bitpack_fitsu(uint64_t n, unsigned width)
{
        if (width >= 64)
                return true;
        /* thanks to Jai Karve and John Bryan */
        return shr(n, width) == 0; // clever shortcut instead of 2 shifts
}
static inline uint64_t Bitpack_getu(uint64_t word, unsigned width, 
                                                                unsigned lsb)
{
        unsigned hi = lsb + width; /* one beyond the most significant bit */
        assert(hi <= 64);
        /* different type of right shift */
        return shr(shl(word, 64 - hi), 64 - width); 
}
static inline uint64_t Bitpack_newu(uint64_t word, unsigned width, 
                                                 unsigned lsb, uint64_t value)
{
        unsigned hi = lsb + width; /* one beyond the most significant bit */
        assert(hi <= 64);
        if (!Bitpack_fitsu(value, width))
                RAISE(Bitpack_Overflow);
        return shl(shr(word, hi), hi)                 /* high part */
                | shr(shl(word, 64 - lsb), 64 - lsb)  /* low part  */
                | (value << lsb);                     /* new part  */
}
/******************************************************************************/


/************************ Segmented Memory Module *****************************/

/* Frees each word within a segment. */
static inline void free_segment(segmentedMem segMem, UMsegment_ID segIndex)
{
        Segment seg = segMem->segmentArr[segIndex];
        if (seg == NULL)
            return;

        segMem->segmentArr[segIndex] = NULL;

        free(seg);
}

/* Returns a new segmentedMem with two sequences, both empty */
static inline segmentedMem new_segmentedMem()
{
        segmentedMem segMem = malloc(sizeof(*segMem));
        assert(segMem != NULL);

        segMem->arraySize = 2;
        segMem->numSegments = 0;
        segMem->segmentArr = malloc(segMem->arraySize * sizeof(Segment));
        segMem->unmappedSegs = Seq_new(5);
        
        return segMem;
}

/* Increases the size of our Segment Array by 2 */
static inline void resizeSegArray(segmentedMem segMem)
{
        int newSize = 2 * segMem->arraySize * sizeof(Segment);
        segMem->segmentArr = realloc(segMem->segmentArr, newSize);
        segMem->arraySize *= 2;
}

/* 
 * Creates a new memory segment of size numWords, returning the index of that
 * segment. If an unmapped segment exists, it will create a new segment in that
 * location. The number of words in a segment is stored in the 0th element of 
 * each segment.
 */
static inline UMsegment_ID map_segment(segmentedMem segMem, unsigned numWords)
{       
        UMsegment_ID segID;
        Segment newSeg = calloc((numWords + 1), sizeof(UMword));
        newSeg[0] = numWords;

        if (Seq_length(segMem->unmappedSegs) > 0){ 
                UMsegment_ID *removedSegID = Seq_remlo(segMem->unmappedSegs);
                segID = *removedSegID;

                free(removedSegID);

                segMem->segmentArr[segID] = newSeg;
        
                return segID;
        }

        if (segMem->numSegments >= segMem->arraySize)
                resizeSegArray(segMem);

        segMem->segmentArr[segMem->numSegments++] = newSeg;
        return segMem->numSegments - 1;
}

/*
 * Frees segmented memory at location segIndex and pops that segIndex onto 
 * the unmappedSegs sequence.
 */
static inline void unmap_segment(segmentedMem segMem, UMsegment_ID segIndex)
{       
        free_segment(segMem, segIndex);

        UMsegment_ID *newID = malloc(sizeof(*newID));
        assert(newID != NULL);
        *newID = segIndex;

        Seq_addhi(segMem->unmappedSegs, newID);
}


/* 
 * Returns the word found at the location [segIndex][wordIndex] within our 
 * segmentArr
 */
static inline UMword segmented_load(segmentedMem segMem, UMsegment_ID segIndex, 
                                                       UMword_ID wordIndex)
{
        return segMem->segmentArr[segIndex][wordIndex + 1];
}


/* Stores the input word at the location [segIndex][wordIndex] in segmentArr */
static inline void segmented_store(segmentedMem segMem, UMsegment_ID segIndex,
                                        UMword_ID wordIndex, UMword word)
{
        segMem->segmentArr[segIndex][wordIndex + 1] = word;
}

static inline Segment segment_copy(Segment oldSeg)
{
        Segment newSeg = calloc((oldSeg[0] + 1), sizeof(UMword));

        unsigned numWords =  oldSeg[0] + 1;

        for (unsigned i = 0; i < numWords; i++)
                newSeg[i] = oldSeg[i];

        return newSeg;
}

/* 
 * Moves the segment at segIndex to segment [0], where our 'program' exists.
 * Unmaps segIndex.
 */
static inline void load_program(segmentedMem segMem, UMsegment_ID segIndex)
{
        if (segIndex == 0)
                return;

        Segment ptrSeg = segMem->segmentArr[segIndex];

        Segment duplicatedSeg = segment_copy(ptrSeg);


        Segment zerothSeg = segMem->segmentArr[0];

        free(zerothSeg);

        segMem->segmentArr[0] = duplicatedSeg;
}

/*
 * Frees the segmentedMem, first freeing the segment sequence then the unmapped
 * sequence.
 */
static inline void free_segmentedMem(segmentedMem segMem)
{      
        int arrayLength = segMem->numSegments;

        while (arrayLength--)
                free_segment(segMem, arrayLength);


        uint32_t unmappedSeqLen = (uint32_t) Seq_length(segMem->unmappedSegs);

        while (unmappedSeqLen--) {
                UMsegment_ID *removedSegID = Seq_remlo(segMem->unmappedSegs);
                free(removedSegID);
        }

        free((segMem->segmentArr));
        Seq_free(&(segMem->unmappedSegs));

        free(segMem);
}

/*****************************************************************************/


/*
 * This function performs the necessary operations for each instruction of the
 * UM (binary extraction to execution).
 */
static inline void emulator(segmentedMem segMem)
{
        UMsegment_ID programCount = 0;
        uint32_t r [8] = {0, 0, 0, 0, 0, 0, 0, 0};
        Um_Opcode opcode = 0;
        while(opcode != HALT)
        {
                uint32_t a, b, c;
                int output, input;
                UMword word = segmented_load(segMem, 0, programCount);
                opcode = Bitpack_getu(word, 4, 28);

                if (opcode != 13) {
                        a = Bitpack_getu(word, 3, 6);                        
                        b = Bitpack_getu(word, 3, 3);
                        c = Bitpack_getu(word, 3, 0);
                } else {
                        a = Bitpack_getu(word, 3, 25);
                        b = Bitpack_getu(word, 25, 0); 
                        (void)c;
                }
                if (opcode == LV)
                        r[a] = b;
                else if (opcode == SLOAD)
                        r[a] = segmented_load(segMem, r[b], r[c]);
                else if (opcode == SSTORE)
                        segmented_store(segMem, r[a], r[b], r[c]);
                else if (opcode == NAND)
                        r[a] = ~(r[b] & r[c]);
                else if (opcode == LOADP) {
                        load_program(segMem, r[b]);
                        programCount = r[c] - 1;
                }
                else if (opcode == ADD)
                        r[a] = (r[b] + r[c]);
                else if (opcode == CMOV) {
                        if (r[c] != 0)
                                r[a] = r[b];
                }
                else if (opcode == ACTIVATE)
                        r[b] = map_segment(segMem, r[c]);
                else if (opcode == INACTIVATE)
                        unmap_segment(segMem, r[c]);
                else if (opcode == DIV)
                        r[a] = r[b] / r[c];
                else if (opcode == MUL)
                        r[a] = (r[b] * r[c]);
                else if (opcode == OUT) {
                        output = r[c];
                        putchar(output);
                }
                else if (opcode == IN) { 
                        input = getchar();
                        if (input == EOF)
                                r[c] = ~0;
                        else 
                                r[c] = input;
                }
                else if(opcode > 13){
                        fprintf(stderr, "Bad instruction.\n");
                        exit(1);
                }
                programCount++;
        }
}

/* 
 * Reads in binary input and loads UM program into segment[0] of our segmented
 * memory.
 */
static inline void programLoader(segmentedMem segMem, FILE *fp, int fileSize)
{
        unsigned numWords = fileSize;

        map_segment(segMem, numWords);

        segmented_store(segMem, 0, 0xffffffff, numWords);
        
        for (unsigned wordNum = 0; wordNum < numWords; wordNum++) {
                uint64_t word = 0;
                for (int lsb = 24; lsb >= 0; lsb -= 8) {
                        unsigned char byte = getc(fp);
                        word = Bitpack_newu(word, 8, lsb, byte);
                }
                segmented_store(segMem, 0, wordNum, word);
        }
}

int main(int argc, char *argv[])
{
        if (argc != 2){
                fprintf(stderr, "Incorrect input format\nUsage) ./um " 
                                "example.um\n");
                exit(1);
        }

        struct stat fileinfo;

        if (stat(argv[1], &fileinfo) < 0) {
                fprintf(stderr, "Incorrect input format.\n");
                exit(1);
        }

        FILE *fp = fopen(argv[1], "r");
        assert(fp != NULL);

        segmentedMem segMem = new_segmentedMem();

        programLoader(segMem, fp, (int) fileinfo.st_size / 4);
        
        emulator(segMem);

        free_segmentedMem(segMem);
        fclose(fp);

        (void) argc;
        return 0;
}