/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables,
 *	  which never need to be WAL-logged or checkpointed, etc.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*#define LBDEBUG*/

/* entry for buffer lookup hashtable */
typedef struct
{
	BufferTag	key;			/* Tag of a disk page */
	int			id;				/* Associated local buffer's index */
} LocalBufferLookupEnt;

/* Note: this macro only works on local buffers, not shared ones! */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

int			NLocBuffer = 0;		/* until buffers are initialized */

BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
int32	   *LocalRefCount = NULL;

static int	nextFreeLocalBuf = 0;

static HTAB *LocalBufHash = NULL;


static void InitLocalBuffers(void);


/*
 * LocalBufferAlloc -
 *	  Find or create a local buffer for the given page of the given relation.
 *
 * API is similar to bufmgr.c's BufferAlloc, except that we do not need
 * to do any locking since this is all local.	Also, IO_IN_PROGRESS
 * does not get set.
 */
BufferDesc *
LocalBufferAlloc(Relation reln, BlockNumber blockNum, bool *foundPtr)
{
	BufferTag	newTag;			/* identity of requested block */
	LocalBufferLookupEnt *hresult;
	BufferDesc *bufHdr;
	int			b;
	int			trycounter;
	bool		found;

	INIT_BUFFERTAG(newTag, reln, blockNum);

	/* Initialize local buffers if first request in this session */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* See if the desired buffer already exists */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, (void *) &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		b = hresult->id;
		bufHdr = &LocalBufferDescriptors[b];
		Assert(BUFFERTAGS_EQUAL(bufHdr->tag, newTag));
#ifdef LBDEBUG
		fprintf(stderr, "LB ALLOC (%u,%d) %d\n",
				RelationGetRelid(reln), blockNum, -b - 1);
#endif

		LocalRefCount[b]++;
		ResourceOwnerRememberBuffer(CurrentResourceOwner,
									BufferDescriptorGetBuffer(bufHdr));
		if (bufHdr->flags & BM_VALID)
			*foundPtr = TRUE;
		else
		{
			/* Previous read attempt must have failed; try again */
			*foundPtr = FALSE;
		}
		return bufHdr;
	}

#ifdef LBDEBUG
	fprintf(stderr, "LB ALLOC (%u,%d) %d\n",
			RelationGetRelid(reln), blockNum, -nextFreeLocalBuf - 1);
#endif

	/*
	 * Need to get a new buffer.  We use a clock sweep algorithm (essentially
	 * the same as what freelist.c does now...)
	 */
	trycounter = NLocBuffer;
	for (;;)
	{
		b = nextFreeLocalBuf;

		if (++nextFreeLocalBuf >= NLocBuffer)
			nextFreeLocalBuf = 0;

		bufHdr = &LocalBufferDescriptors[b];

		if (LocalRefCount[b] == 0 && bufHdr->usage_count == 0)
		{
			LocalRefCount[b]++;
			ResourceOwnerRememberBuffer(CurrentResourceOwner,
										BufferDescriptorGetBuffer(bufHdr));
			break;
		}

		if (bufHdr->usage_count > 0)
		{
			bufHdr->usage_count--;
			trycounter = NLocBuffer;
		}
		else if (--trycounter == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no empty local buffer available")));
	}

	/*
	 * this buffer is not referenced but it might still be dirty. if that's
	 * the case, write it out before reusing it!
	 */
	if (bufHdr->flags & BM_DIRTY)
	{
		SMgrRelation oreln;

		/* Find smgr relation for buffer */
		oreln = smgropen(bufHdr->tag.rnode);

		/* And write... */
		smgrwrite(oreln,
				  bufHdr->tag.blockNum,
				  (char *) LocalBufHdrGetBlock(bufHdr),
				  true);

		/* Mark not-dirty now in case we error out below */
		bufHdr->flags &= ~BM_DIRTY;

		LocalBufferFlushCount++;
	}

	/*
	 * lazy memory allocation: allocate space on first use of a buffer.
	 */
	if (LocalBufHdrGetBlock(bufHdr) == NULL)
	{
		char	   *data;

		data = (char *) MemoryContextAlloc(TopMemoryContext, BLCKSZ);

		/* Set pointer for use by BufferGetBlock() macro */
		LocalBufHdrGetBlock(bufHdr) = (Block) data;
	}

	/*
	 * Update the hash table: remove old entry, if any, and make new one.
	 */
	if (bufHdr->flags & BM_TAG_VALID)
	{
		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, (void *) &bufHdr->tag,
						HASH_REMOVE, NULL);
		if (!hresult)			/* shouldn't happen */
			elog(ERROR, "local buffer hash table corrupted");
		/* mark buffer invalid just in case hash insert fails */
		CLEAR_BUFFERTAG(bufHdr->tag);
		bufHdr->flags &= ~(BM_VALID | BM_TAG_VALID);
	}

	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, (void *) &newTag, HASH_ENTER, &found);
	if (found)					/* shouldn't happen */
		elog(ERROR, "local buffer hash table corrupted");
	hresult->id = b;

	/*
	 * it's all ours now.
	 */
	bufHdr->tag = newTag;
	bufHdr->flags &= ~(BM_VALID | BM_DIRTY | BM_JUST_DIRTIED | BM_IO_ERROR);
	bufHdr->flags |= BM_TAG_VALID;
	bufHdr->usage_count = 0;

	*foundPtr = FALSE;
	return bufHdr;
}

/*
 * WriteLocalBuffer -
 *	  writes out a local buffer (actually, just marks it dirty)
 */
void
WriteLocalBuffer(Buffer buffer, bool release)
{
	int			bufid;
	BufferDesc *bufHdr;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB WRITE %d\n", buffer);
#endif

	bufid = -(buffer + 1);

	Assert(LocalRefCount[bufid] > 0);

	bufHdr = &LocalBufferDescriptors[bufid];
	bufHdr->flags |= BM_DIRTY;

	if (release)
	{
		LocalRefCount[bufid]--;
		if (LocalRefCount[bufid] == 0 &&
			bufHdr->usage_count < BM_MAX_USAGE_COUNT)
			bufHdr->usage_count++;
		ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
	}
}

/*
 * DropRelFileNodeLocalBuffers
 *		This function removes from the buffer pool all the pages of the
 *		specified relation that have block numbers >= firstDelBlock.
 *		(In particular, with firstDelBlock = 0, all pages are removed.)
 *		Dirty pages are simply dropped, without bothering to write them
 *		out first.	Therefore, this is NOT rollback-able, and so should be
 *		used only with extreme caution!
 *
 *		See DropRelFileNodeBuffers in bufmgr.c for more notes.
 */
void
DropRelFileNodeLocalBuffers(RelFileNode rnode, BlockNumber firstDelBlock)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = &LocalBufferDescriptors[i];
		LocalBufferLookupEnt *hresult;

		if ((bufHdr->flags & BM_TAG_VALID) &&
			RelFileNodeEquals(bufHdr->tag.rnode, rnode) &&
			bufHdr->tag.blockNum >= firstDelBlock)
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %u/%u/%u is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 bufHdr->tag.rnode.spcNode,
					 bufHdr->tag.rnode.dbNode,
					 bufHdr->tag.rnode.relNode,
					 LocalRefCount[i]);
			/* Remove entry from hashtable */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, (void *) &bufHdr->tag,
							HASH_REMOVE, NULL);
			if (!hresult)		/* shouldn't happen */
				elog(ERROR, "local buffer hash table corrupted");
			/* Mark buffer invalid */
			CLEAR_BUFFERTAG(bufHdr->tag);
			bufHdr->flags = 0;
			bufHdr->usage_count = 0;
		}
	}
}

/*
 * InitLocalBuffers -
 *	  init the local buffer cache. Since most queries (esp. multi-user ones)
 *	  don't involve local buffers, we delay allocating actual memory for the
 *	  buffers until we need them; just make the buffer headers here.
 */
static void
InitLocalBuffers(void)
{
	int			nbufs = num_temp_buffers;
	HASHCTL		info;
	int			i;

	/* Allocate and zero buffer headers and auxiliary arrays */
	LocalBufferDescriptors = (BufferDesc *) calloc(nbufs, sizeof(BufferDesc));
	LocalBufferBlockPointers = (Block *) calloc(nbufs, sizeof(Block));
	LocalRefCount = (int32 *) calloc(nbufs, sizeof(int32));
	if (!LocalBufferDescriptors || !LocalBufferBlockPointers || !LocalRefCount)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	nextFreeLocalBuf = 0;

	/* initialize fields that need to start off nonzero */
	for (i = 0; i < nbufs; i++)
	{
		BufferDesc *buf = &LocalBufferDescriptors[i];

		/*
		 * negative to indicate local buffer. This is tricky: shared buffers
		 * start with 0. We have to start with -2. (Note that the routine
		 * BufferDescriptorGetBuffer adds 1 to buf_id so our first buffer id
		 * is -1.)
		 */
		buf->buf_id = -i - 2;
	}

	/* Create the lookup hash table */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(LocalBufferLookupEnt);
	info.hash = tag_hash;

	LocalBufHash = hash_create("Local Buffer Lookup Table",
							   nbufs,
							   &info,
							   HASH_ELEM | HASH_FUNCTION);

	if (!LocalBufHash)
		elog(ERROR, "could not initialize local buffer hash table");

	/* Initialization done, mark buffers allocated */
	NLocBuffer = nbufs;
}

/*
 * AtEOXact_LocalBuffers - clean up at end of transaction.
 *
 * This is just like AtEOXact_Buffers, but for local buffers.
 */
void
AtEOXact_LocalBuffers(bool isCommit)
{
#ifdef USE_ASSERT_CHECKING
	if (assert_enabled)
	{
		int			i;

		for (i = 0; i < NLocBuffer; i++)
		{
			Assert(LocalRefCount[i] == 0);
		}
	}
#endif
}

/*
 * AtProcExit_LocalBuffers - ensure we have dropped pins during backend exit.
 *
 * This is just like AtProcExit_Buffers, but for local buffers.  We have
 * to drop pins to ensure that any attempt to drop temp files doesn't
 * fail in DropRelFileNodeBuffers.
 */
void
AtProcExit_LocalBuffers(void)
{
	/* just zero the refcounts ... */
	if (LocalRefCount)
		MemSet(LocalRefCount, 0, NLocBuffer * sizeof(*LocalRefCount));
}
