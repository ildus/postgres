#include "postgres.h"
#include "tsearch/ts_type.h"

/*
 * Defition of old WordEntry struct in TSVector. Because of limitations
 * in size (max 1MB for lexemes), a format have changed
 */
typedef struct
{
	uint32
				haspos:1,
				len:11,
				pos:20;
} OldWordEntry;

typedef struct
{
	uint16		npos;
	WordEntryPos pos[FLEXIBLE_ARRAY_MEMBER];
} OldWordEntryPosVector;

#define _OLDPOSVECPTR(x, e)	\
	((OldWordEntryPosVector *)(STRPTR(x) + SHORTALIGN((e)->pos + (e)->len)))
#define OLDPOSDATALEN(x,e) ( ( (e)->haspos ) ? (_OLDPOSVECPTR(x,e)->npos) : 0 )
#define OLDPOSDATAPTR(x,e) (_OLDPOSVECPTR(x,e)->pos)

TSVector
tsvector_upgrade(Datum orig)
{
	bool       writable;
	WordEntry *newentry;
	int	       i,
	           offset;
	TSVector   vec = (TSVector) PG_DETOAST_DATUM(orig);

	/* If already in new format, return as is */
	if (vec->size_ & TS_FLAG_EXPANDED)
		return vec;

	/* We got an old format, converting to the new */
	writable = ((void *) vec != (void *) DatumGetPointer(orig));
	if (!writable)
		vec = (TSVector) PG_DETOAST_DATUM_COPY(orig);

	/* Contains current position in new tsvector */
	offset = 0;

	for (i = 0; i < vec->size_; i++)
	{
		/* Save old values */
		OldWordEntry entry = *((OldWordEntry *)(vec->entries + i));

		/* Fill new fields */
		newentry = vec->entries + i;
		newentry->len = entry.len;
		newentry->npos = OLDPOSDATALEN(vec, &entry);
		newentry->_unused = 0;

		/* Move lexeme */
		offset = SHORTALIGN(offset);
		memmove(STRPTR(vec) + offset, STRPTR(vec) + entry.pos, entry.len);
		offset += entry.len;

		/* Move positions */
		if (entry.haspos)
		{
			offset = SHORTALIGN(offset);
			memmove(STRPTR(vec) + offset, OLDPOSDATAPTR(vec, &entry),
				sizeof(WordEntryPos) * newentry->npos);
			offset += sizeof(WordEntryPos) * newentry->npos;
		}
	}

	/* New size will be less than old size if tsvector had positions */
	SET_VARSIZE(vec, CALCDATASIZE(vec->size_, offset));

	/*
	 * Mark TSVector as new, after this operation vec->size will
	 *	contain special flag
	 */
	TS_SETCOUNT(vec, vec->size_);
	return vec;
}
