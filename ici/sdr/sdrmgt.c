/*
 *	sdrmgt.c:	simple data recorder database space
 *			management library.
 *
 *	Copyright (c) 2001-2007, California Institute of Technology.
 *	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
 *	acknowledged.
 *
 *	Author: Scott Burleigh, JPL
 *
 *	This library implements management of some portion of a
 *	spacecraft data recording resource that is to be configured
 *	and accessed as a fixed-size pool of notionally non-volatile
 *	memory with a flat address space.
 *
 *	Modification History:
 *	Date	  Who	What
 *	4-3-96	  APS	Abstracted IPC services and task control.
 *	5-1-96	  APS	Ported to sparc-sunos4.
 *	12-20-00  SCB	Revised for sparc-sunos5.
 *	6-8-07    SCB	Divided sdr.c library into separable components.
 */

#include "sdrP.h"
#include "lyst.h"
#include "sdrmgt.h"

typedef struct
{
	Address		from;	/*	1st byte of object		*/
	Address		to;	/*	1st byte beyond scope of object	*/
} ObjectExtent;

/*		--	SDR space management stuff.	--		*/

#if SPACE_ORDER == 3
#define	SMALL_IN_USE	(0xffffffffffffff00)
#define	SMALL_NEXT_ADDR (0x00ffffffffffffff)
#define	LARGE_IN_USE	(0xffffffffffffffff)
#else
#define	SMALL_IN_USE	(0xffffff00)
#define	SMALL_NEXT_ADDR (0x00ffffff)
#define	LARGE_IN_USE	(0xffffffff)
#endif

#define	SMALL_BLOCK_OHD	(WORD_SIZE)
#define	SMALL_BLK_LIMIT	(SMALL_SIZES * WORD_SIZE)
#define SMALL_MAX_ADDR	(1L << (8 * (WORD_SIZE - 1)))

/*
 * The overhead on a small block is WORD_SIZE bytes.  The last byte
 * of this overhead always contains the size of the block's user data
 * (expressed as an integer 1 through SMALL_SIZES: the total block
 * size minus overhead, divided by WORD_SIZE).  When the block is in
 * use, the leading bytes all contain 0xff.  When the block is free,
 * the leading bytes contain the low-order bytes of the Address of
 * the next free block, which must be an integral multiple of
 * WORD_SIZE; the high-order byte of that address is zero, which
 * implies that the maximum size of a small pool on a 32-bit machine
 * is 16 Megabytes less sizeof(SdrMap) because free blocks beyond
 * that location wouldn't be addressable in 24 bits.
 */

typedef struct
{
	u_long	next;
} SmallOhd;

/*
 * The overhead on a large block is in two parts that MUST BE THE
 * SAME SIZE, a leading overhead area and a trailing overhead area.
 * The first word of the leading overhead contains the size of the
 * block's user data.  The first word of the trailing overhead
 * contains the Address of the leading overhead.  When the block
 * is in use, the second word of the leading overhead and the
 * second word of the trailing overhead both contain LARGE_IN_USE;
 * when the block is free, the second word of the leading overhead
 * contains the Address of the next free block and the second word
 * of the trailing overhead contains the Address of the preceding
 * free block.
 *
 * To ensure correct alignment of the trailing overhead area, the
 * user data area in a large block must be an integral multiple of
 * the leading and trailing overhead areas' size.
 */

typedef struct
{
	u_long	userDataSize;		/*	in bytes, not words	*/
	Address	next;			/*	(BigOhd1)		*/
} BigOhd1;				/*	leading overhead	*/

typedef struct
{
	Address	start;			/*	(of block's BigOhd1)	*/
	Address	prev;			/*	(BigOhd1)		*/
} BigOhd2;				/*	trailing overhead	*/

#define	LG_OHD_SIZE	(1 << LARGE_ORDER1)	/*	double word	*/
#define	LARGE_BLOCK_OHD	(2 * LG_OHD_SIZE)
#define	MIN_LARGE_BLOCK	(3 * LG_OHD_SIZE)
#define	LARGE_BLK_LIMIT	(1UL << LARGE_ORDERn)

typedef enum { NotAnObject, SmallObject, LargeObject } ObjectScale;

typedef union
{
	SmallOhd	wee;
	BigOhd1		leading;
	BigOhd2		trailing;
} Ohd;

/*		Space management utility functions.			*/

static ObjectScale	scaleOf(Sdr sdrv, Address addr, Ohd *ohd)
{
	SdrMap	*map;
	Address	leader;
	Address	trailer;
	BigOhd2	trailing;

	/*	Small objects are visible only within the SDR library
		itself; we assume the SDR functions themselves are
		reliable, so we don't validate small-object addresses.
		But we can ensure that they are in fact objects, that
		is, that they are currently allocated rather than on
		the relevant free list.

		Large objects are publicly accessible, so we validate
		the address of a purported large object by ensuring
		that its trailing overhead points back to its leading
		overhead.  In addition, we ensure that it is indeed
		an object by verifying that its overhead words indicate
		that the object is LARGE_IN_USE.			*/

	map = _mapImage(sdrv);
	if (addr >= map->startOfSmallPool
	&& addr < map->endOfSmallPool)
	{
		leader = addr - SMALL_BLOCK_OHD;
		sdrFetch(ohd->wee, leader);
		if ((ohd->wee.next & SMALL_IN_USE) != SMALL_IN_USE)
		{
			return NotAnObject;
		}

		return SmallObject;
	}

	if (addr >= (map->startOfLargePool + LG_OHD_SIZE)
	&& addr < map->endOfLargePool)
	{
		leader = addr - LG_OHD_SIZE;
		sdrFetch(ohd->leading, leader);
		trailer = addr + ohd->leading.userDataSize;
		if (trailer > addr
		&& trailer + LG_OHD_SIZE <= sdrv->sdr->sdrSize)
		{
			sdrFetch(trailing, trailer);
			if (trailing.start == leader)
			{
				if (ohd->leading.next != LARGE_IN_USE
				|| trailing.prev != LARGE_IN_USE)
				{
					return NotAnObject;
				}

				return LargeObject;
			}
		}
	}

	return NotAnObject;
}

static LystElt	noteKnownObject(Sdr sdrv, Address from, Address to)
{
	ObjectExtent	*extent;

	extent = (ObjectExtent *) MTAKE(sizeof(ObjectExtent));
	if (extent == NULL)
	{
		return NULL;
	}

	extent->from = from;
	extent->to = to;
	return lyst_insert_last(sdrv->knownObjects, extent);
}

void	sdr_stage(Sdr sdrv, char *into, Object from, long length)
{
	SdrState	*sdr;
	Address		addr = (Address) from;
	Address		to;
	Ohd		ohd;
	LystElt		elt;
	ObjectExtent	*extent;

	CHKVOID(sdr_in_xn(sdrv));
	XNCHKVOID(length == 0 || (length > 0 && into != NULL));
	XNCHKVOID(from);
	sdr = sdrv->sdr;
	if ((sdr->configFlags & SDR_BOUNDED) == 0)/*	No staging.	*/
	{
		sdr_read(sdrv, into, from, length);
		return;
	}

	to = addr + length;
	if (addr < 0 || to < addr || (to == addr && length != 0)
			|| to > sdr->sdrSize)
	{
		putErrmsg(_violationMsg(), "stage");
		crashXn(sdrv);
		return;
	}

	if (scaleOf(sdrv, addr, &ohd) != LargeObject)
	{
		putErrmsg("Can't stage data, not a user object.", NULL);
		crashXn(sdrv);
		return;
	}

	for (elt = lyst_first(sdrv->knownObjects); elt; elt = lyst_next(elt))
	{
		extent = (ObjectExtent *) lyst_data(elt);
		if (extent->from == addr)	/*	Already staged.	*/
		{
			break;
		}
	}

	if (elt == 0)		/*	Not staged yet.			*/
	{
		if (noteKnownObject(sdrv, addr,
			addr + ohd.leading.userDataSize) == NULL)
		{
			putErrmsg(_noMemoryMsg(), NULL);
			crashXn(sdrv);
			return;
		}
	}

	/*	Length may be zero, in which case the object is just
		staged for subsequent update.				*/

	if (length)
	{
		sdr_read(sdrv, into, from, length);
	}
}

/*	*	*	Trace management functions	*	*	*/

int	sdr_start_trace(Sdr sdrv, long shmSize, char *shm)
{
#ifndef SDR_TRACE
	putErrmsg(_noTraceMsg(), NULL);
	return -1;
#else
	SdrState	*sdr;

	CHKERR(sdrv);
	sdr = sdrv->sdr;
	CHKERR(takeSdr(sdr) == 0);
	if (shmSize < 1)	/*	Must allocate some space.	*/
        {
		releaseSdr(sdr);
		putErrmsg("Need some shared memory to start trace.", NULL);
		return -1;
	}

	if (sdr->traceSize > 0)	/*	Trace is already enabled.	*/
	{
		if (sdr->traceSize != shmSize)
        	{
			releaseSdr(sdr);
			putErrmsg("Asserted trace memory size doesn't match \
actual.", NULL);
			return -1;
		}
	}
	else			/*	Trace is not currently enabled.	*/
	{
		sdr->traceSize = shmSize;	/*	Enable trace.	*/
	}

	sdrv->trace = &(sdrv->traceArea);

	/*	(To prevent dynamic allocation of the trace episode's
	 *	space management structure.)				*/

	sdrv->trace = sptrace_start(sdr->traceKey, sdr->traceSize, shm,
			sdrv->trace, sdr->name);
	if (sdrv->trace == NULL)
	{
		releaseSdr(sdr);
		putErrmsg("Can't start sdr trace.", NULL);
		return -1;
	}

	releaseSdr(sdr);
	sdrv->currentSourceFileName = NULL;
	sdrv->currentSourceFileLine = 0;
	return 0;
#endif
}

void	sdr_print_trace(Sdr sdrv, int verbose)
{
#ifndef SDR_TRACE
	return;
#else
	SdrState	*sdr;
	SdrUsageSummary	summary;

	CHKVOID(sdrv);
	sdr = sdrv->sdr;
	CHKVOID(takeSdr(sdr) == 0);
	sptrace_report(sdrv->trace, verbose);
	sdr_usage(sdrv, &summary);
	sdr_report(&summary);
	releaseSdr(sdr);
#endif
}

void	sdr_clear_trace(Sdr sdrv)
{
#ifndef SDR_TRACE
	return;
#else
	SdrState	*sdr;

	CHKVOID(sdrv);
	sdr = sdrv->sdr;
	CHKVOID(takeSdr(sdr) == 0);
	sptrace_clear(sdrv->trace);
	releaseSdr(sdr);
#endif
}

void	sdr_stop_trace(Sdr sdrv)
{
#ifndef SDR_TRACE
	return;
#else
	SdrState	*sdr;

	CHKVOID(sdrv);
	sdr = sdrv->sdr;
	CHKVOID(takeSdr(sdr) == 0);
	if (sdrv->trace)
	{
		sptrace_stop(sdrv->trace);
		sdrv->trace = NULL;
	}

	sdrv->sdr->traceSize = 0;		/*	Disable trace.	*/
	releaseSdr(sdr);
#endif
}

void	joinTrace(Sdr sdrv, char *sourceFileName, int lineNbr)
{
#ifndef SDR_TRACE
	return;
#else
	SdrState	*sdr;

	CHKVOID(sdrv);
	sdr = sdrv->sdr;
	if (sdrv->trace == NULL)
	{
		if (sdr->traceSize < 1)	/*	Trace is disabled.	*/
		{
			return;		/*	Fails silently.		*/
		}

		if (sdr_start_trace(sdrv, sdr->traceSize, NULL) < 0)
		{
			return;		/*	Fails silently.		*/
		}
	}
	else	/*	Already joined; still valid?			*/
	{
		if (sdr->traceSize < 1)	/*	Trace is now disabled.	*/
		{
			sdrv->trace = NULL;
			return;
		}
	}

	sdrv->currentSourceFileName = sourceFileName;
	sdrv->currentSourceFileLine = lineNbr;
#endif
}

/*	*	*	Space management functions	*	*	*/

Object	_sdrzalloc(Sdr sdrv, unsigned long nbytes)
{
	long		i;
	Address		ohdAddress;
	Address		newFirst;
	SmallOhd	ohd;
	long		userDataWords;
	SdrMap		*map;
	long		increment;
	Object		result;
	Address		newEnd;
	u_long		newUnassigned;

	CHKZERO(sdrv);
	XNCHKZERO(!(nbytes == 0 || nbytes > SMALL_BLK_LIMIT));

	/*	Increase nbytes to align it properly: must
		be an integral multiple of SMALL_BLOCK_OHD.
		So we round up by adding (SMALL_BLOCK_OHD - 1)
		and then dropping the low-order bits.  In the
		process, we also compute bucket number i.		*/

	nbytes += (SMALL_BLOCK_OHD - 1);
	nbytes >>= SPACE_ORDER;
	i = nbytes - 1;	/*	(But nbytes is actual nbr of words)	*/
	nbytes <<= SPACE_ORDER;
	userDataWords = i + 1;

	/*	At this point nbytes is correctly aligned and
		we have the correct bucket number.			*/

	CHKZERO(sdrv);
	map = _mapImage(sdrv);
	ohdAddress = map->firstSmallFree[i];
	if (ohdAddress != 0)
	{
	/*	Free small block is available for allocation.		*/

		sdrFetch(ohd, ohdAddress);
		newFirst = (ohd.next >> 8) & SMALL_NEXT_ADDR;
		patchMap(firstSmallFree[i], newFirst);
		ohd.next = SMALL_IN_USE + userDataWords;
		sdrPatch(ohdAddress, ohd);
		result = (Object) (ohdAddress + SMALL_BLOCK_OHD);
#ifdef SDR_TRACE
		sptrace_log_alloc(sdrv->trace, result, nbytes,
				sdrv->currentSourceFileName,
				sdrv->currentSourceFileLine);
#endif
		return result;
	}

	/*	No free block available, must create one if possible.	*/

	increment = nbytes + SMALL_BLOCK_OHD;
	if (map->unassignedSpace < increment
	|| (map->endOfSmallPool + increment) > SMALL_MAX_ADDR)
	{
		putErrmsg("No space left in small pool.", NULL);
		crashXn(sdrv);
		return 0;
	}

	/*	Heap has unassigned space, so we can extend small pool.	*/

	ohdAddress = map->endOfSmallPool;
	newEnd = ohdAddress + increment;
	patchMap(endOfSmallPool, newEnd);
	newUnassigned = map->unassignedSpace - increment;
	patchMap(unassignedSpace, newUnassigned);
	ohd.next = SMALL_IN_USE + userDataWords;
	sdrPatch(ohdAddress, ohd);
	result = (Object) (ohdAddress + SMALL_BLOCK_OHD);
#ifdef SDR_TRACE
	sptrace_log_alloc(sdrv->trace, result, nbytes,
			sdrv->currentSourceFileName,
			sdrv->currentSourceFileLine);
#endif
	return result;
}

static int	computeBucket(u_int userDataSize)
{
	u_int	highOrderBits;
	int	bucket;

	highOrderBits = userDataSize >> (LARGE_ORDER1 + 1);
	bucket = 0;
	while (highOrderBits)
	{
		bucket++;
		highOrderBits >>= 1;
	}

	return bucket;
}

static void	insertFreeBlock(Sdr sdrv, Address leader, Address trailer)
{
	SdrMap	*map = _mapImage(sdrv);
	BigOhd1	leading;
	BigOhd2	trailing;
	int	bucket;
	Address	nextLeader;
	BigOhd1	nextLeading;
	Address	nextTrailer;
	BigOhd2	nextTrailing;

	sdrFetch(leading, leader);
	sdrFetch(trailing, trailer);
	bucket = computeBucket(leading.userDataSize);
	sdrFetch(nextLeader, ADDRESS_OF(firstLargeFree[bucket]));
	if (nextLeader == 0)
	{
		leading.next = 0;
	}
	else
	{
		leading.next = nextLeader;
		sdrFetch(nextLeading, nextLeader);
		nextTrailer = nextLeader + sizeof(BigOhd1) +
				nextLeading.userDataSize;
		sdrFetch(nextTrailing, nextTrailer);
		nextTrailing.prev = leader;
		sdrPatch(nextTrailer, nextTrailing);
	}

	sdrPatch(leader, leading);
	trailing.prev = 0;
	sdrPatch(trailer, trailing);
	patchMap(firstLargeFree[bucket], leader);
}

static void	removeFromBucket(Sdr sdrv, int bucket, Address leader,
				Address trailer)
{
	SdrMap	*map = _mapImage(sdrv);
	BigOhd1	leading;
	BigOhd2	trailing;
	Address	nextLeader;
	BigOhd1	nextLeading;
	Address	nextTrailer;
	BigOhd2	nextTrailing;
	Address	prevLeader;
	BigOhd1	prevLeading;

	sdrFetch(leading, leader);
	sdrFetch(trailing, trailer);
	if ((nextLeader = leading.next) != 0)		/*	!last	*/
	{
		sdrFetch(nextLeading, nextLeader);
		nextTrailer = nextLeader + sizeof(BigOhd1)
				+ nextLeading.userDataSize;
		sdrFetch(nextTrailing, nextTrailer);
		nextTrailing.prev = trailing.prev;
		sdrPatch(nextTrailer, nextTrailing);
	}

	if ((prevLeader = trailing.prev) == 0)		/*	1st.	*/
	{
		patchMap(firstLargeFree[bucket], nextLeader);
	}
	else						/*	!1st.	*/
	{
		sdrFetch(prevLeading, prevLeader);
		prevLeading.next = nextLeader;
		sdrPatch(prevLeader, prevLeading);
	}

	leading.next = LARGE_IN_USE;
	sdrPatch(leader, leading);
	trailing.prev = LARGE_IN_USE;
	sdrPatch(trailer, trailing);
}

static Object	mallocLarge(Sdr sdrv, unsigned long nbytes)
{
	SdrMap		*map;
	int		bucket;
	Address		leader = 0;
	BigOhd1		leading;
	Address		trailer;
	BigOhd2		trailing;
	Address		newStart;
	long		newUnassigned;
	u_long		increment;
	Address		newLeader;
	BigOhd1		newLeading;
	Address		newTrailer;
	BigOhd2		newTrailing;
	u_long		surplus;

	/*	Increase nbytes to align it properly: must be an
		integral multiple of LG_OHD_SIZE (which is equal to
		2^LARGE_ORDER1, i.e., 1 << LARGE_ORDER1).		*/

	nbytes += (LG_OHD_SIZE - 1);	/*	to force carry if nec.	*/
	nbytes >>= LARGE_ORDER1;	/*	truncate		*/
	nbytes <<= LARGE_ORDER1;	/*	restore size		*/
	map = _mapImage(sdrv);

	/*	Determine which bucket a block of this size would be
		stored in if free, but then (unless nbytes is the
		minimum block size for that bucket) allocate from the
		NEXT bucket because every free block in that bucket is
		certain to be at least of size nbytes -- so you can
		take the first one instead of searching for a match.	*/

	bucket = computeBucket(nbytes);
	if (nbytes != (1 << (bucket + LARGE_ORDER1)))
	{
		bucket++;
	}

	while (bucket < LARGE_ORDERS
	&& (leader = map->firstLargeFree[bucket]) == 0)
	{
		bucket++;
	}

	if (leader)	/*	Found free block that's big enough.	*/
	{
		sdrFetch(leading, leader);
	}
	else	/*	Need to increase pool size.			*/
	{
		increment = nbytes + LARGE_BLOCK_OHD;
		if (map->unassignedSpace >= increment)
		{
			newStart = map->startOfLargePool - increment;
			patchMap(startOfLargePool, newStart);
			newUnassigned = map->unassignedSpace - increment;
			patchMap(unassignedSpace, newUnassigned);
			leader = newStart;	/*	Created block.	*/
			leading.userDataSize = nbytes;
			leading.next = LARGE_IN_USE;
			sdrPatch(leader, leading);
			trailer = leader + sizeof(BigOhd1) + nbytes;
			trailing.start = leader;
			trailing.prev = LARGE_IN_USE;
			sdrPatch(trailer, trailing);
			return (Object) (leader + LG_OHD_SIZE);
		}

		/*	Can't allocate block from unassigned space.	*/

		putErrmsg("Can't increase large pool size.", NULL);
		crashXn(sdrv);
		return 0;
	}

	/*	Free block found.  Must remove from bucket.		*/

	trailer = leader + sizeof(BigOhd1) + leading.userDataSize;
	sdrFetch(trailing, trailer);
	removeFromBucket(sdrv, bucket, leader, trailer);

	/*	Split off surplus, if any, as separate free block.	*/

	surplus = leading.userDataSize - nbytes;
	if (surplus >= MIN_LARGE_BLOCK)	/*	Must bisect block.	*/
	{
		/*	Shorten original block.				*/

		leading.userDataSize = nbytes;
		leading.next = LARGE_IN_USE;

		/*	Note: although removeFromBucket has already
			changed to LARGE_IN_USE the "next" field of
			the block's overhead, the local "leading"
			buffer in stack has not been similarly
			updated.  We have to do it here to avoid
			hosing the block's overhead when it is
			rewritten.					*/

		sdrPatch(leader, leading);
		newTrailer = leader + sizeof(BigOhd1) + nbytes;
		newTrailing.start = leader;
		newTrailing.prev = LARGE_IN_USE;
		sdrPatch(newTrailer, newTrailing);

		/*	Make new block out of surplus.			*/

		newLeader = newTrailer + sizeof(BigOhd2);
		newLeading.userDataSize = surplus - LARGE_BLOCK_OHD;
		newLeading.next = LARGE_IN_USE;
		sdrPatch(newLeader, newLeading);
		trailing.start = newLeader;
		sdrPatch(trailer, trailing);
		insertFreeBlock(sdrv, newLeader, trailer);
	}

	return (Object) (leader + LG_OHD_SIZE);
}

Object	_sdrmalloc(Sdr sdrv, unsigned long nbytes)
{
	SdrState	*sdr = sdrv->sdr;
	Object		object;
	Address		addr;
	Ohd		ohd;

	CHKZERO(sdrv);
	XNCHKZERO(!(nbytes == 0 || nbytes > LARGE_BLK_LIMIT));
	object = mallocLarge(sdrv, nbytes);
	if (object != 0)
	{
		if (sdr->configFlags & SDR_BOUNDED)
		{
			memset((char *) &ohd, 0, sizeof(Ohd));
			addr = (Address) object;
			oK(scaleOf(sdrv, addr, &ohd));
			if (noteKnownObject(sdrv, addr,
				addr + ohd.leading.userDataSize) == NULL)
			{
				putErrmsg(_noMemoryMsg(), NULL);
				crashXn(sdrv);
				return 0;
			}
		}

#ifdef SDR_TRACE
		sptrace_log_alloc(sdrv->trace, object, nbytes,
				sdrv->currentSourceFileName,
				sdrv->currentSourceFileLine);
#endif
	}

	return object;
}

Object	Sdr_malloc(char *file, int line, Sdr sdrv, unsigned long nbytes)
{
	if (!(sdr_in_xn(sdrv)))
	{
		oK(_iEnd(file, line, _notInXnMsg()));
		return 0;
	}

	joinTrace(sdrv, file, line);
	return _sdrmalloc(sdrv, nbytes);
}

Object	Sdr_insert(char *file, int line, Sdr sdrv, char *from,
		unsigned long size)
{
	Object	obj;

	if (!(sdr_in_xn(sdrv)))
	{
		oK(_iEnd(file, line, _notInXnMsg()));
		return 0;
	}

	joinTrace(sdrv, file, line);
	obj = _sdrmalloc(sdrv, size);
	if (obj)
	{
		_sdrput(file, line, sdrv, (Address) obj, from, size, SystemPut);
	}

	return obj;
}

static void	freeLarge(Sdr sdrv, Address addr)
{
	SdrMap	*map = _mapImage(sdrv);
	Address	leader;
	BigOhd1	leading;
	Address	trailer;
	BigOhd2	trailing;
	Address	endOfLargePool;
	Address	startOfLargePool;
	Address	nextLeader;
	BigOhd1	nextLeading;
	Address	nextTrailer;
	int	bucket;
	BigOhd2	nextTrailing;
	Address	prevTrailer;
	BigOhd2	prevTrailing;
	Address	prevLeader;
	BigOhd1	prevLeading;

	leader = addr - LG_OHD_SIZE;
	sdrFetch(leading, leader);
	trailer = addr + leading.userDataSize;
	sdrFetch(trailing, trailer);
	sdrFetch(endOfLargePool, ADDRESS_OF(endOfLargePool));
	sdrFetch(startOfLargePool, ADDRESS_OF(startOfLargePool));

	/*	Consolidate with physically subsequent block if free.	*/

	nextLeader = trailer + sizeof(BigOhd2);
	if (nextLeader < endOfLargePool)
	{
		sdrFetch(nextLeading, nextLeader);
		if (nextLeading.next != LARGE_IN_USE)	/*	Free.	*/
		{
			nextTrailer = nextLeader + sizeof(BigOhd1)
					+ nextLeading.userDataSize;
			sdrFetch(nextTrailing, nextTrailer);
			bucket = computeBucket(nextLeading.userDataSize);
			removeFromBucket(sdrv, bucket, nextLeader, nextTrailer);

			/*	Concatenate with block being freed.
				Trailer of subsequent block becomes
				the trailer of the aggregated block.	*/

			leading.userDataSize +=
				(nextLeading.userDataSize + LARGE_BLOCK_OHD);
			sdrPatch(leader, leading);
			nextTrailing.start = leader;
			sdrPatch(nextTrailer, nextTrailing);
			memcpy((char *) &trailing, (char *) &nextTrailing,
					sizeof(BigOhd2));
			trailer = nextTrailer;
		}
	}

	/*	Consolidate with physically prior block if free.	*/

	if (leader > startOfLargePool)
	{
		prevTrailer = leader - sizeof(BigOhd2);
		sdrFetch(prevTrailing, prevTrailer);
		if (prevTrailing.prev != LARGE_IN_USE)	/*	Free.	*/
		{
			prevLeader = prevTrailing.start;
			sdrFetch(prevLeading, prevLeader);
			bucket = computeBucket(prevLeading.userDataSize);
			removeFromBucket(sdrv, bucket, prevLeader, prevTrailer);

			/*	Concatenate with block being freed.
				Leader of prior block becomes the
				leader of the aggregated block.		*/

			prevLeading.userDataSize += (leading.userDataSize
					+ LARGE_BLOCK_OHD);
			sdrPatch(prevLeader, prevLeading);
			trailing.start = prevLeader;
			sdrPatch(trailer, trailing);
			memcpy((char *) &leading, (char *) &prevLeading,
					sizeof(BigOhd1));
			leader = prevLeader;
		}
	}

	/*	Insert the (possibly consolidated) free block.		*/

	insertFreeBlock(sdrv, leader, trailer);
}

void	_sdrfree(Sdr sdrv, Object object, PutSrc src)
{
	SdrState	*sdr;
	SdrMap		*map = _mapImage(sdrv);
	Address		addr = (Address) object;
	Address		block;
	Ohd		ohd;
	long		userDataWords;
	long		i;
	unsigned long	next;
	LystElt		elt;
	ObjectExtent	*extent;

	CHKVOID(sdrv);
	sdr = sdrv->sdr;
	switch (scaleOf(sdrv, addr, &ohd))
	{
	case SmallObject:	/*	For SDR library use only.	*/
		if (src == UserPut)
		{
#ifdef SDR_TRACE
			sptrace_log_memo(sdrv->trace, object,
				"attempt to free an SDR system object",
				sdrv->currentSourceFileName,
				sdrv->currentSourceFileLine);
#endif
			putErrmsg("Can't free SDR private object.", NULL);
			crashXn(sdrv);
			return;
		}

		/*	Select appropriate small free space stack
			and push object's space onto that stack.	*/

		userDataWords = ohd.wee.next & 0xff;
		i = userDataWords - 1;
		sdrFetch(next, ADDRESS_OF(firstSmallFree[i]));
		ohd.wee.next = (next << 8) + userDataWords;
		block = addr - SMALL_BLOCK_OHD;
		sdrPatch(block, ohd);
		patchMap(firstSmallFree[i], block);
		break;

	case LargeObject:
		block = addr - LG_OHD_SIZE;
		freeLarge(sdrv, addr);
		if ((sdr->configFlags & SDR_BOUNDED) == 0)
		{
			break;
		}

		/*	Ensure object isn't in transaction's list of
			knownObjects any more.				*/

		for (elt = lyst_first(sdrv->knownObjects); elt;
				elt = lyst_next(elt))
		{
			extent = (ObjectExtent *) lyst_data(elt);
			if (addr == extent->from)
			{
				lyst_delete(elt);
				break;
			}
		}

		break;

	default:
#ifdef SDR_TRACE
		sptrace_log_memo(sdrv->trace, object,
				"attempt to free a nonexistent object",
				sdrv->currentSourceFileName,
				sdrv->currentSourceFileLine);
#endif
		putErrmsg("Can't free arbitrary space.", NULL);
		crashXn(sdrv);
		return;
	}

#ifdef SDR_TRACE
	sptrace_log_free(sdrv->trace, object, sdrv->currentSourceFileName,
			sdrv->currentSourceFileLine);
#endif
}

void	Sdr_free(char *file, int line, Sdr sdrv, Object object)
{
	if (!(sdr_in_xn(sdrv)))
	{
		oK(_iEnd(file, line, _notInXnMsg()));
		return;
	}

	joinTrace(sdrv, file, line);
	_sdrfree(sdrv, object, UserPut);
}

/*	*	Space management utility functions	*	*	*/

int	sdrBoundaryViolated(Sdr sdrv, Address from, long length)
{
	Address		to;
	LystElt		elt;
	ObjectExtent	*extent;

	to = from + length;
	for (elt = lyst_first(sdrv->knownObjects); elt; elt = lyst_next(elt))
	{
		extent = (ObjectExtent *) lyst_data(elt);
		if (extent->from <= from || extent->to >= to)
		{
			/*	First byte written is within this
				object and the last byte written is
				within the same object; write is okay.	*/

			return 0;
		}
	}

	/*	No known object was found that encompasses this write.	*/

	return 1;
}

long	sdr_object_length(Sdr sdrv, Object object)
{
	Address	addr = (Address) object;
	Ohd	ohd;

	CHKERR(sdrv);
	switch (scaleOf(sdrv, addr, &ohd))
	{
	case SmallObject:
		return WORD_SIZE * (ohd.wee.next & 0xff);

	case LargeObject:
		return ohd.leading.userDataSize;

	default:
		return -1;
	}
}

long	sdr_unused(Sdr sdrv)
{
	SdrMap		*map;
	long		smallPoolSize;
	long		largePoolSize;
	long		unused;

	CHKZERO(sdrFetchSafe(sdrv));
	map = _mapImage(sdrv);
	unused = map->sdrSize - sizeof(SdrMap);
	smallPoolSize = map->endOfSmallPool - map->startOfSmallPool;
	largePoolSize = map->endOfLargePool - map->startOfLargePool;
       	unused -= (smallPoolSize + largePoolSize);
	return unused;
}

void	sdr_usage(Sdr sdrv, SdrUsageSummary *usage)
{
	SdrState	*sdr;
	SdrMap		*map;
	int		i;
	u_long		size;
	Address		smallBlk;
	SmallOhd	smallHead;
	Address		nextSmall;
	Address		large;
	BigOhd1		largeHead;
	Address		nextLarge;
	u_long		freeTotal;
	int		count;

	CHKVOID(sdrFetchSafe(sdrv));
	CHKVOID(usage);
	sdr = sdrv->sdr;
	istrcpy(usage->sdrName, sdr->name, sizeof usage->sdrName);
	usage->sdrSize = sdr->sdrSize;
	map = _mapImage(sdrv);
	usage->smallPoolSize = map->endOfSmallPool - map->startOfSmallPool;
	freeTotal = 0;
	size = 0;
	for (i = 0; i < SMALL_SIZES; i++)
	{
		size += WORD_SIZE;
		count = 0;
		for (smallBlk = (Address) (map->firstSmallFree[i]); smallBlk;
				smallBlk = nextSmall)
		{
			sdrFetch(smallHead, smallBlk);
			nextSmall = (Address)
				((smallHead.next >> 8) & SMALL_NEXT_ADDR);
			count++;
		}

		freeTotal += (count * size);
		usage->smallPoolFreeBlockCount[i] = count;
	}

	usage->smallPoolFree = freeTotal;
	usage->smallPoolAllocated = usage->smallPoolSize - freeTotal;
	usage->largePoolSize = map->endOfLargePool - map->startOfLargePool;
	freeTotal = 0;
	for (i = 0; i < LARGE_ORDERS; i++)
	{
		count = 0;
		for (large = (Address) (map->firstLargeFree[i]); large;
				large = nextLarge)
		{
			sdrFetch(largeHead, large);
			nextLarge = (Address) (largeHead.next);
			freeTotal += largeHead.userDataSize;
			count++;
		}

		usage->largePoolFreeBlockCount[i] = count;
	}

	usage->largePoolFree = freeTotal;
	usage->largePoolAllocated = usage->largePoolSize - freeTotal;
	usage->unusedSize = usage->sdrSize - (sizeof(SdrMap) +
			 usage->smallPoolSize + usage->largePoolSize);
	return;
}

void	sdr_report(SdrUsageSummary *usage)
{
	int	i;
	u_long	size;
	int	count;
	char	buf[100];

	CHKVOID(usage);
	isprintf(buf, sizeof buf, "-- sdr '%s' usage report --",
			usage->sdrName);
	writeMemo(buf);
	size = 0;
	istrcpy(buf, "small pool free blocks:", sizeof buf);
	writeMemo(buf);
	for (i = 0; i < SMALL_SIZES; i++)
	{
		size += WORD_SIZE;
		count = usage->smallPoolFreeBlockCount[i];
		if (count > 0)
		{
			isprintf(buf, sizeof buf, "    %10d of size %10ld",
					count, size);
	                writeMemo(buf);
		}
	}

	isprintf(buf, sizeof buf, "       total avbl: %10ld",
			usage->smallPoolFree);
	writeMemo(buf);
	isprintf(buf, sizeof buf, "     total unavbl: %10ld",
			usage->smallPoolAllocated);
	writeMemo(buf);
	isprintf(buf, sizeof buf, "       total size: %10ld",
			usage->smallPoolSize);
	writeMemo(buf);
	size = WORD_SIZE;
	istrcpy(buf, "large pool free blocks:", sizeof buf);
	writeMemo(buf);
	for (i = 0; i < LARGE_ORDERS; i++)
	{
		size *= 2;
		count = usage->largePoolFreeBlockCount[i];
		if (count > 0)
		{
			isprintf(buf, sizeof buf, "    %10d of order %10ld",
					count, size);
	                writeMemo(buf);
		}
	}

	isprintf(buf, sizeof buf, "       total avbl: %10ld",
			usage->largePoolFree);
        writeMemo(buf);
	isprintf(buf, sizeof buf, "     total unavbl: %10ld",
		       	usage->largePoolAllocated);
        writeMemo(buf);
	isprintf(buf, sizeof buf, "       total size: %10ld",
		       	usage->largePoolSize);
        writeMemo(buf);
	isprintf(buf, sizeof buf, "total sdr:         %10ld",
		       	usage->sdrSize);
        writeMemo(buf);
	isprintf(buf, sizeof buf, "total unused:      %10ld",
		       	usage->unusedSize);
        writeMemo(buf);
}

int	sdr_heap_depleted(Sdr sdrv)
{
	SdrUsageSummary	summary;

	CHKERR(sdrv);
	CHKERR(sdrFetchSafe(sdrv));
	sdr_usage(sdrv, &summary);
	return ((summary.smallPoolFree + summary.largePoolFree
			+ summary.unusedSize) < (summary.sdrSize / 16));
}
