/*-------------------------------------------------------------------------
 *
 * ts_expand.c
 *	  Expanded tsvector functions
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_expand.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "tsearch/ts_utils.h"

#define EV_MAGIC 0xF4F4F4F4

static Size EV_get_flat_size(ExpandedObjectHeader *eohptr);
static void EV_flatten_into(ExpandedObjectHeader *eohptr,
	void *result, Size allocated_size);

static const ExpandedObjectMethods EV_methods =
{
	EV_get_flat_size,
	EV_flatten_into
};

static void
EV_flatten_into(ExpandedObjectHeader *eohptr, void *result,
	Size allocated_size)
{
	ExpandedTSVectorHeader *evhptr = (ExpandedTSVectorHeader *)eohptr;
	memcpy(result, PG_DETOAST_DATUM(evhptr->datum), allocated_size);
}

static Size
EV_get_flat_size(ExpandedObjectHeader *eohptr)
{
	ExpandedTSVectorHeader *evhptr = (ExpandedTSVectorHeader *)eohptr;
	return VARSIZE(evhptr->datum);
}


/*
 * expand_tsvector - converts tsvector datum to an expanded
 * tsvector
 */
static Datum
expand_tsvector(Datum vectorDatum, MemoryContext parentcontext)
{
	MemoryContext objcxt, oldcxt;
	ExpandedTSVectorHeader *evh;
	TSVector vector;

	/* If already expanded do nothing */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(vectorDatum)))
		return vectorDatum;

	/* Allocate private context for expanded object */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded tsvector",
								   ALLOCSET_SMALL_MINSIZE,
								   ALLOCSET_SMALL_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);

	/* Set up expanded tsvector header */
	evh = (ExpandedTSVectorHeader *)
		MemoryContextAlloc(objcxt, sizeof(ExpandedTSVectorHeader));

	EOH_init_header(&evh->hdr, &EV_methods, objcxt);

	/* Get only size_ value of TSVector */
	vector = (TSVector) PG_DETOAST_DATUM_SLICE(vectorDatum, 0, sizeof(int32));

	oldcxt = MemoryContextSwitchTo(objcxt);
	evh->ev_magic = EV_MAGIC;
	evh->datum = vectorDatum;
	evh->count = TS_COUNT(vector);
	evh->maxidx = 0;
	evh->positions = (uint32 *)palloc(sizeof(uint32) * evh->count);
	evh->positions[0] = 0;
	evh->entries = (WordEntry *) VARDATA(PG_DETOAST_DATUM_SLICE(vectorDatum,
		sizeof(int32), sizeof(WordEntry) * evh->count));

	MemoryContextSwitchTo(oldcxt);
	pfree(vector);

	/* return a R/O pointer to the expanded tsvector */
	return EOHPGetRODatum(&evh->hdr);
}

/* Returns lexeme and its entry from expanded TSVector */
char *
tsvector_getlexeme(TSVectorExpanded vec, int idx, WordEntry **we)
{
	char		   *lexeme;
	MemoryContext	oldcxt;
	int32			pos,
					pos2;

	Assert(idx >=0 && idx < vec->count);

	/*
	 * we do not allow we == NULL because returned lexeme is not \0 ended,
	 * and always should be used with we->len
	 */
	Assert(we != NULL);
	*we = &vec->entries[idx];

	/* Calculate count for slice */
	pos2 = pos = tsvector_getposition(vec, idx);
	pos2 += (*we)->len;
	if ((*we)->npos)
	{
		pos2 = SHORTALIGN(pos2);
		pos2 += (*we)->npos * sizeof(WordEntryPos);
	}

	/* we do all memory allocations in the context of the expanded object */
	oldcxt = MemoryContextSwitchTo(vec->hdr.eoh_context);
	lexeme = (char *) VARDATA(PG_DETOAST_DATUM_SLICE(vec->datum,
		sizeof(int32) + sizeof(WordEntry) * vec->count + pos,
		pos2 - pos + 1));
	MemoryContextSwitchTo(oldcxt);

	return lexeme;
}

/* Returns position for index, and calculates positions cache if needed */
int
tsvector_getposition(TSVectorExpanded vec, int idx)
{
	WordEntry *entry;
	int32 i, pos;

	if (idx > vec->maxidx)
	{
		pos = vec->positions[vec->maxidx];
		entry = &vec->entries[vec->maxidx];

		for (i = vec->maxidx + 1; i <= idx; i++)
		{
			IncrPtr(entry, pos);
			vec->positions[i] = pos;
		}
		vec->maxidx = idx;
	}

	/* Calculate count for slice */
	return vec->positions[idx];
}

ExpandedTSVectorHeader *
DatumGetExpandedTSVector(Datum d)
{
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(d)))
	{
		ExpandedTSVectorHeader *evh =
			(ExpandedTSVectorHeader *) DatumGetEOHP(d);

		Assert(evh->ev_magic == EV_MAGIC);
		return evh;
	}

	/* Else expand the hard way */
	d = expand_tsvector(d, CurrentMemoryContext);
	return (ExpandedTSVectorHeader *) DatumGetEOHP(d);
}

