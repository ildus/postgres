/*-------------------------------------------------------------------------
 *
 * tsvector.c
 *	  I/O functions for tsvector
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsvector.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

typedef struct
{
	WordEntry		entry;			/* must be first! */
	size_t			offset;			/* offset of lexeme in some buffer */
	WordEntryPos   *pos;
} WordEntryIN;


/* Compare two WordEntryPos values for qsort */
int
compareWordEntryPos(const void *a, const void *b)
{
	int			apos = WEP_GETPOS(*(const WordEntryPos *) a);
	int			bpos = WEP_GETPOS(*(const WordEntryPos *) b);

	if (apos == bpos)
		return 0;
	return (apos > bpos) ? 1 : -1;
}

/*
 * Removes duplicate pos entries. If there's two entries with same pos
 * but different weight, the higher weight is retained.
 *
 * Returns new length.
 */
static int
uniquePos(WordEntryPos *a, int l)
{
	WordEntryPos *ptr,
			   *res;

	if (l <= 1)
		return l;

	qsort((void *) a, l, sizeof(WordEntryPos), compareWordEntryPos);

	res = a;
	ptr = a + 1;
	while (ptr - a < l)
	{
		if (WEP_GETPOS(*ptr) != WEP_GETPOS(*res))
		{
			res++;
			*res = *ptr;
			if (res - a >= MAXNUMPOS - 1 ||
				WEP_GETPOS(*res) == MAXENTRYPOS - 1)
				break;
		}
		else if (WEP_GETWEIGHT(*ptr) > WEP_GETWEIGHT(*res))
			WEP_SETWEIGHT(*res, WEP_GETWEIGHT(*ptr));
		ptr++;
	}

	return res + 1 - a;
}

/* Compare two WordEntryIN values for qsort */
static int
compareentry(const void *va, const void *vb, void *arg)
{
	const WordEntryIN *a = (const WordEntryIN *) va;
	const WordEntryIN *b = (const WordEntryIN *) vb;
	char	   *BufferStr = (char *) arg;

	return tsCompareString(&BufferStr[a->offset], a->entry.len,
						   &BufferStr[b->offset], b->entry.len,
						   false);
}

/*
 * Sort an array of WordEntryIN, remove duplicates.
 * *outbuflen receives the amount of space needed for strings and positions.
 */
static int
uniqueentry(WordEntryIN *a, int l, char *buf, int *outbuflen)
{
	int			buflen;
	WordEntryIN *ptr,
			   *res;

	Assert(l >= 1);

	if (l > 1)
		qsort_arg((void *) a, l, sizeof(WordEntryIN), compareentry,
				  (void *) buf);

	buflen = 0;
	res = a;
	ptr = a + 1;
	while (ptr - a < l)
	{
		if (!(ptr->entry.len == res->entry.len &&
			  strncmp(&buf[ptr->offset], &buf[res->offset], res->entry.len) == 0))
		{
			/* done accumulating data into *res, count space needed */
			buflen += res->entry.len;
			if (res->entry.npos)
			{
				res->entry.npos = uniquePos(res->pos, res->entry.npos);
				buflen = SHORTALIGN(buflen);
				buflen += res->entry.npos * sizeof(WordEntryPos);
			}
			res++;
			if (res != ptr)
				memcpy(res, ptr, sizeof(WordEntryIN));
		}
		else if (ptr->entry.npos)
		{
			if (res->entry.npos)
			{
				/* append ptr's positions to res's positions */
				int newlen = ptr->entry.npos + res->entry.npos;

				res->pos = (WordEntryPos *)
					repalloc(res->pos, newlen * sizeof(WordEntryPos));
				memcpy(&res->pos[res->entry.npos], ptr->pos,
					   ptr->entry.npos * sizeof(WordEntryPos));
				res->entry.npos = newlen;
				pfree(ptr->pos);
			}
			else
			{
				/* just give ptr's positions to pos */
				res->entry.npos = ptr->entry.npos;
				res->pos = ptr->pos;
			}
		}
		ptr++;
	}

	/* count space needed for last item */
	buflen += res->entry.len;
	if (res->entry.npos)
	{
		res->entry.npos = uniquePos(res->pos, res->entry.npos);
		buflen = SHORTALIGN(buflen);
		buflen += res->entry.npos * sizeof(WordEntryPos);
	}

	*outbuflen = buflen;
	return res + 1 - a;
}

static int
WordEntryCMP(WordEntry *a, WordEntry *b, char *buf)
{
	return compareentry(a, b, buf);
}


Datum
tsvectorin(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	TSVectorParseState state;
	WordEntryIN *arr;
	int			totallen;
	int			arrlen;			/* allocated size of arr */
	WordEntry  *inarr;
	int			len = 0;
	TSVector	in;
	int			i;
	char	   *token;
	int			toklen;
	WordEntryPos *pos;
	int			poslen;
	char	   *strbuf;
	int			stroff;

	/*
	 * Tokens are appended to tmpbuf, cur is a pointer to the end of used
	 * space in tmpbuf.
	 */
	char	   *tmpbuf;
	char	   *cur;
	int			buflen = 256;	/* allocated size of tmpbuf */

	state = init_tsvector_parser(buf, false, false);

	arrlen = 64;
	arr = (WordEntryIN *) palloc(sizeof(WordEntryIN) * arrlen);
	cur = tmpbuf = (char *) palloc(buflen);

	while (gettoken_tsvector(state, &token, &toklen, &pos, &poslen, NULL))
	{
		if (toklen >= MAXSTRLEN)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long (%ld bytes, max %ld bytes)",
							(long) toklen,
							(long) (MAXSTRLEN - 1))));

		if (cur - tmpbuf > MAXSTRPOS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("string is too long for tsvector (%ld bytes, max %ld bytes)",
							(long) (cur - tmpbuf), (long) MAXSTRPOS)));

		/*
		 * Enlarge buffers if needed
		 */
		if (len >= arrlen)
		{
			arrlen *= 2;
			arr = (WordEntryIN *)
				repalloc((void *) arr, sizeof(WordEntryIN) * arrlen);
		}
		while ((cur - tmpbuf) + toklen >= buflen)
		{
			int			dist = cur - tmpbuf;

			buflen *= 2;
			tmpbuf = (char *) repalloc((void *) tmpbuf, buflen);
			cur = tmpbuf + dist;
		}
		arr[len].entry.len = toklen;
		arr[len].offset = cur - tmpbuf;
		arr[len].entry.npos = poslen;
		arr[len].pos = (poslen != 0)? pos : NULL;
		memcpy((void *) cur, (void *) token, toklen);
		cur += toklen;
		len++;
	}

	close_tsvector_parser(state);

	if (len > 0)
		len = uniqueentry(arr, len, tmpbuf, &buflen);
	else
		buflen = 0;

	if (buflen > MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", buflen, MAXSTRPOS)));

	totallen = CALCDATASIZE(len, buflen);
	in = (TSVector) palloc0(totallen);
	SET_VARSIZE(in, totallen);
	TS_SETCOUNT(in, len);
	inarr = ARRPTR(in);
	strbuf = STRPTR(in);
	stroff = 0;
	for (i = 0; i < len; i++)
	{
		memcpy(strbuf + stroff, &tmpbuf[arr[i].offset], arr[i].entry.len);
		stroff += arr[i].entry.len;
		if (arr[i].entry.npos)
		{
			if (arr[i].entry.npos > 0xFFFF)
				elog(ERROR, "positions array too long");

			/* Copy number of positions */
			stroff = SHORTALIGN(stroff);

			/* Copy positions */
			memcpy(strbuf + stroff, arr[i].pos, arr[i].entry.npos * sizeof(WordEntryPos));
			stroff += arr[i].entry.npos * sizeof(WordEntryPos);

			pfree(arr[i].pos);
		}
		inarr[i] = arr[i].entry;
	}

	Assert((strbuf + stroff - (char *) in) == totallen);

	PG_RETURN_TSVECTOR(in);
}

Datum
tsvectorout(PG_FUNCTION_ARGS)
{
	TSVector	out = PG_GETARG_TSVECTOR(0);
	char	   *outbuf;
	int32		i,
				lenbuf = 0,
				pos = 0,
				pp,
				tscount = TS_COUNT(out);
	WordEntry  *ptr = ARRPTR(out);
	char	   *curbegin,
			   *curin,
			   *curout;

	lenbuf = tscount * 2 /* '' */ + tscount - 1 /* space */ + 2 /* \0 */ ;
	for (i = 0; i < tscount; i++)
	{
		lenbuf += ptr[i].len * 2 * pg_database_encoding_max_length() /* for escape */ ;
		if (ptr[i].npos)
			lenbuf += 1 /* : */ + 7 /* int2 + , + weight */ * ptr[i].npos;
	}

	curout = outbuf = (char *) palloc(lenbuf);
	for (i = 0; i < tscount; i++)
	{
		curbegin = curin = STRPTR(out) + pos;
		if (i != 0)
			*curout++ = ' ';
		*curout++ = '\'';
		while (curin - curbegin < ptr->len)
		{
			int			len = pg_mblen(curin);

			if (t_iseq(curin, '\''))
				*curout++ = '\'';
			else if (t_iseq(curin, '\\'))
				*curout++ = '\\';

			while (len--)
				*curout++ = *curin++;
		}

		*curout++ = '\'';
		if ((pp = ptr->npos) != 0)
		{
			WordEntryPos *wptr;

			*curout++ = ':';
			wptr = POSDATAPTR(curbegin, ptr->len);
			while (pp)
			{
				curout += sprintf(curout, "%d", WEP_GETPOS(*wptr));
				switch (WEP_GETWEIGHT(*wptr))
				{
					case 3:
						*curout++ = 'A';
						break;
					case 2:
						*curout++ = 'B';
						break;
					case 1:
						*curout++ = 'C';
						break;
					case 0:
					default:
						break;
				}

				if (pp > 1)
					*curout++ = ',';
				pp--;
				wptr++;
			}
		}
		INCRPTR(ptr, pos);
	}

	*curout = '\0';
	PG_FREE_IF_COPY(out, 0);
	PG_RETURN_CSTRING(outbuf);
}

/*
 * Binary Input / Output functions. The binary format is as follows:
 *
 * uint32	number of lexemes
 *
 * for each lexeme:
 *		lexeme text in client encoding, null-terminated
 *		uint16	number of positions
 *		for each position:
 *			uint16 WordEntryPos
 */

Datum
tsvectorsend(PG_FUNCTION_ARGS)
{
	TSVector	vec = PG_GETARG_TSVECTOR(0);
	StringInfoData buf;
	int			i,
				j,
				pos = 0;
	WordEntry  *weptr = ARRPTR(vec);

	pq_begintypsend(&buf);

	pq_sendint(&buf, TS_COUNT(vec), sizeof(int32));
	for (i = 0; i < TS_COUNT(vec); i++)
	{
		char *lexeme = STRPTR(vec) + pos;

		/*
		 * the strings in the TSVector array are not null-terminated, so we
		 * have to send the null-terminator separately
		 */
		pq_sendtext(&buf, lexeme, weptr->len);
		pq_sendbyte(&buf, '\0');

		pq_sendint(&buf, weptr->npos, sizeof(uint16));

		if (weptr->npos > 0)
		{
			WordEntryPos *wepptr = POSDATAPTR(lexeme, weptr->len);

			for (j = 0; j < weptr->npos; j++)
				pq_sendint(&buf, wepptr[j], sizeof(WordEntryPos));
		}
		INCRPTR(weptr, pos);
	}

	PG_FREE_IF_COPY(vec, 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tsvectorrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TSVector	vec;
	int			i;
	int32		nentries;
	int			datalen;		/* number of bytes used in the variable size
								 * area after fixed size TSVector header and
								 * WordEntries */
	Size		hdrlen;
	Size		len;			/* allocated size of vec */
	bool		needSort = false;

	nentries = pq_getmsgint(buf, sizeof(int32));
	if (nentries < 0 || nentries > (MaxAllocSize / sizeof(WordEntry)))
		elog(ERROR, "invalid size of tsvector");

	hdrlen = DATAHDRSIZE + sizeof(WordEntry) * nentries;

	len = hdrlen * 2;			/* times two to make room for lexemes */
	vec = (TSVector) palloc0(len);
	TS_SETCOUNT(vec, nentries);

	datalen = 0;
	for (i = 0; i < nentries; i++)
	{
		const char *lexeme;
		char *lexeme_out;
		uint16		npos;
		size_t		lex_len;

		lexeme = pq_getmsgstring(buf);
		npos = (uint16) pq_getmsgint(buf, sizeof(uint16));

		/* sanity checks */

		lex_len = strlen(lexeme);
		if (lex_len > MAXSTRLEN)
			elog(ERROR, "invalid tsvector: lexeme too long");

		if (npos > MAXNUMPOS)
			elog(ERROR, "unexpected number of tsvector positions");

		/*
		 * Looks valid. Fill the WordEntry struct, and copy lexeme.
		 *
		 * But make sure the buffer is large enough first.
		 */
		while (hdrlen + SHORTALIGN(datalen + lex_len) +
			   (npos + 1) * sizeof(WordEntryPos) >= len)
		{
			len *= 2;
			vec = (TSVector) repalloc(vec, len);
		}

		vec->entries[i].npos = npos;
		vec->entries[i].len = lex_len;

		lexeme_out = STRPTR(vec) + datalen;
		memcpy(lexeme_out, lexeme, lex_len);

		datalen += lex_len;

		if (i > 0 && WordEntryCMP(&vec->entries[i],
								  &vec->entries[i - 1],
								  STRPTR(vec)) <= 0)
			needSort = true;

		/* Receive positions */
		if (npos > 0)
		{
			uint16		j;
			WordEntryPos *wepptr;

			/*
			 * Pad to 2-byte alignment if necessary. Though we used palloc0
			 * for the initial allocation, subsequent repalloc'd memory areas
			 * are not initialized to zero.
			 */
			if (datalen != SHORTALIGN(datalen))
			{
				*(STRPTR(vec) + datalen) = '\0';
				datalen = SHORTALIGN(datalen);
			}

			wepptr = POSDATAPTR(lexeme_out, lex_len);
			for (j = 0; j < npos; j++)
			{
				wepptr[j] = (WordEntryPos) pq_getmsgint(buf, sizeof(WordEntryPos));
				if (j > 0 && WEP_GETPOS(wepptr[j]) <= WEP_GETPOS(wepptr[j - 1]))
					elog(ERROR, "position information is misordered");
			}

			datalen += npos * sizeof(WordEntry);
		}
	}

	SET_VARSIZE(vec, hdrlen + datalen);

	if (needSort)
		qsort_arg((void *) ARRPTR(vec), TS_COUNT(vec), sizeof(WordEntry),
				  compareentry, (void *) STRPTR(vec));

	PG_RETURN_TSVECTOR(vec);
}
