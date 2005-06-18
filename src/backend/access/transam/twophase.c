/*-------------------------------------------------------------------------
 *
 * twophase.c
 *		Two-phase commit support functions.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		$PostgreSQL: pgsql/src/backend/access/transam/twophase.c,v 1.1 2005/06/17 22:32:42 tgl Exp $
 *
 * NOTES
 *		Each global transaction is associated with a global transaction
 *		identifier (GID). The client assigns a GID to a postgres
 *		transaction with the PREPARE TRANSACTION command.
 *
 *		We keep all active global transactions in a shared memory array.
 *		When the PREPARE TRANSACTION command is issued, the GID is
 *		reserved for the transaction in the array. This is done before
 *		a WAL entry is made, because the reservation checks for duplicate
 *		GIDs and aborts the transaction if there already is a global
 *		transaction in prepared state with the same GID.
 *
 *		A global transaction (gxact) also has a dummy PGPROC that is entered
 *		into the ProcArray array; this is what keeps the XID considered
 *		running by TransactionIdIsInProgress.  It is also convenient as a
 *		PGPROC to hook the gxact's locks to.
 *
 *		In order to survive crashes and shutdowns, all prepared
 *		transactions must be stored in permanent storage. This includes
 *		locking information, pending notifications etc. All that state
 *		information is written to the per-transaction state file in
 *		the pg_twophase directory.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/subtrans.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "pgstat.h"


/*
 * Directory where Two-phase commit files reside within PGDATA
 */
#define TWOPHASE_DIR "pg_twophase"

/* GUC variable, can't be changed after startup */
int max_prepared_xacts = 50;

/*
 * This struct describes one global transaction that is in prepared state
 * or attempting to become prepared.
 *
 * The first component of the struct is a dummy PGPROC that is inserted
 * into the global ProcArray so that the transaction appears to still be
 * running and holding locks.  It must be first because we cast pointers
 * to PGPROC and pointers to GlobalTransactionData back and forth.
 *
 * The lifecycle of a global transaction is:
 *
 * 1. After checking that the requested GID is not in use, set up an
 * entry in the TwoPhaseState->prepXacts array with the correct XID and GID,
 * with locking_xid = my own XID and valid = false.
 *
 * 2. After successfully completing prepare, set valid = true and enter the
 * contained PGPROC into the global ProcArray.
 *
 * 3. To begin COMMIT PREPARED or ROLLBACK PREPARED, check that the entry
 * is valid and its locking_xid is no longer active, then store my current
 * XID into locking_xid.  This prevents concurrent attempts to commit or
 * rollback the same prepared xact.
 *
 * 4. On completion of COMMIT PREPARED or ROLLBACK PREPARED, remove the entry
 * from the ProcArray and the TwoPhaseState->prepXacts array and return it to
 * the freelist.
 *
 * Note that if the preparing transaction fails between steps 1 and 2, the
 * entry will remain in prepXacts until recycled.  We can detect recyclable
 * entries by checking for valid = false and locking_xid no longer active.
 *
 * typedef struct GlobalTransactionData *GlobalTransaction appears in 
 * twophase.h
 */
#define GIDSIZE 200

typedef struct GlobalTransactionData
{
	PGPROC		proc;			/* dummy proc */
	AclId		owner;			/* ID of user that executed the xact */
	TransactionId locking_xid;	/* top-level XID of backend working on xact */
	bool		valid;			/* TRUE if fully prepared */
	char gid[GIDSIZE];			/* The GID assigned to the prepared xact */
} GlobalTransactionData;

/*
 * Two Phase Commit shared state.  Access to this struct is protected
 * by TwoPhaseStateLock.
 */
typedef struct TwoPhaseStateData
{
	/* Head of linked list of free GlobalTransactionData structs */
	SHMEM_OFFSET freeGXacts;

	/* Number of valid prepXacts entries. */
	int		numPrepXacts;

	/*
	 * There are max_prepared_xacts items in this array, but C wants a
	 * fixed-size array.
	 */
	GlobalTransaction	prepXacts[1]; /* VARIABLE LENGTH ARRAY */
} TwoPhaseStateData;			/* VARIABLE LENGTH STRUCT */

static TwoPhaseStateData *TwoPhaseState;


static void RecordTransactionCommitPrepared(TransactionId xid,
											int nchildren,
											TransactionId *children,
											int nrels,
											RelFileNode *rels);
static void RecordTransactionAbortPrepared(TransactionId xid,
											int nchildren,
											TransactionId *children,
											int nrels,
											RelFileNode *rels);
static void ProcessRecords(char *bufptr, TransactionId xid,
						   const TwoPhaseCallback callbacks[]);


/*
 * Initialization of shared memory
 */
int
TwoPhaseShmemSize(void)
{
	/* Need the fixed struct, the array of pointers, and the GTD structs */
	return MAXALIGN(offsetof(TwoPhaseStateData, prepXacts) + 
					sizeof(GlobalTransaction) * max_prepared_xacts) +
		sizeof(GlobalTransactionData) * max_prepared_xacts;
}

void
TwoPhaseShmemInit(void)
{
	bool found;

	TwoPhaseState = ShmemInitStruct("Prepared Transaction Table",
									TwoPhaseShmemSize(),
									&found);
	if (!IsUnderPostmaster)
	{
		GlobalTransaction gxacts;
		int			i;

		Assert(!found);
		TwoPhaseState->freeGXacts = INVALID_OFFSET;
		TwoPhaseState->numPrepXacts = 0;

		/*
		 * Initialize the linked list of free GlobalTransactionData structs
		 */
		gxacts = (GlobalTransaction)
			((char *) TwoPhaseState +
			 MAXALIGN(offsetof(TwoPhaseStateData, prepXacts) + 
					  sizeof(GlobalTransaction) * max_prepared_xacts));
		for (i = 0; i < max_prepared_xacts; i++)
		{
			gxacts[i].proc.links.next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = MAKE_OFFSET(&gxacts[i]);
		}
	}
	else
		Assert(found);
}


/*
 * MarkAsPreparing
 * 		Reserve the GID for the given transaction.
 *
 * Internally, this creates a gxact struct and puts it into the active array.
 * NOTE: this is also used when reloading a gxact after a crash; so avoid
 * assuming that we can use very much backend context.
 */
GlobalTransaction
MarkAsPreparing(TransactionId xid, Oid databaseid, char *gid, AclId owner)
{
	GlobalTransaction	gxact;
	int i;

	if (strlen(gid) >= GIDSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("global transaction identifier \"%s\" is too long",
						gid)));

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	/*
	 * First, find and recycle any gxacts that failed during prepare.
	 * We do this partly to ensure we don't mistakenly say their GIDs
	 * are still reserved, and partly so we don't fail on out-of-slots
	 * unnecessarily.
	 */
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		gxact = TwoPhaseState->prepXacts[i];
		if (!gxact->valid && !TransactionIdIsActive(gxact->locking_xid))
		{
			/* It's dead Jim ... remove from the active array */
			TwoPhaseState->numPrepXacts--;
			TwoPhaseState->prepXacts[i] = TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts];
			/* and put it back in the freelist */
			gxact->proc.links.next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = MAKE_OFFSET(gxact);
			/* Back up index count too, so we don't miss scanning one */
			i--;
		}
	}

	/* Check for conflicting GID */
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		gxact = TwoPhaseState->prepXacts[i];
		if (strcmp(gxact->gid, gid) == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("global transaction identifier \"%s\" is already in use",
							gid)));
		}
	}

	/* Get a free gxact from the freelist */
	if (TwoPhaseState->freeGXacts == INVALID_OFFSET)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of prepared transactions reached"),
				 errhint("Increase max_prepared_transactions (currently %d).",
						 max_prepared_xacts)));
	gxact = (GlobalTransaction) MAKE_PTR(TwoPhaseState->freeGXacts);
	TwoPhaseState->freeGXacts = gxact->proc.links.next;

	/* Initialize it */
	MemSet(&gxact->proc, 0, sizeof(PGPROC));
	SHMQueueElemInit(&(gxact->proc.links));
	gxact->proc.waitStatus = STATUS_OK;
	gxact->proc.xid = xid;
	gxact->proc.xmin = InvalidTransactionId;
	gxact->proc.pid = 0;
	gxact->proc.databaseId = databaseid;
	gxact->proc.lwWaiting = false;
	gxact->proc.lwExclusive = false;
	gxact->proc.lwWaitLink = NULL;
	gxact->proc.waitLock = NULL;
	gxact->proc.waitProcLock = NULL;
	SHMQueueInit(&(gxact->proc.procLocks));
	/* subxid data must be filled later by GXactLoadSubxactData */
	gxact->proc.subxids.overflowed = false;
	gxact->proc.subxids.nxids = 0;

	gxact->owner = owner;
	gxact->locking_xid = xid;
	gxact->valid = false;
	strcpy(gxact->gid, gid);

	/* And insert it into the active array */
	Assert(TwoPhaseState->numPrepXacts < max_prepared_xacts);
	TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts++] = gxact;

	LWLockRelease(TwoPhaseStateLock);

	return gxact;
}

/*
 * GXactLoadSubxactData
 *
 * If the transaction being persisted had any subtransactions, this must
 * be called before MarkAsPrepared() to load information into the dummy
 * PGPROC.
 */
static void
GXactLoadSubxactData(GlobalTransaction gxact, int nsubxacts,
					 TransactionId *children)
{
	/* We need no extra lock since the GXACT isn't valid yet */
	if (nsubxacts > PGPROC_MAX_CACHED_SUBXIDS)
	{
		gxact->proc.subxids.overflowed = true;
		nsubxacts = PGPROC_MAX_CACHED_SUBXIDS;
	}
	if (nsubxacts > 0)
	{
		memcpy(gxact->proc.subxids.xids, children,
			   nsubxacts * sizeof(TransactionId));
		gxact->proc.subxids.nxids = nsubxacts;
	}
}

/*
 * MarkAsPrepared
 *		Mark the GXACT as fully valid, and enter it into the global ProcArray.
 */
void
MarkAsPrepared(GlobalTransaction gxact)
{
	/* Lock here may be overkill, but I'm not convinced of that ... */
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	Assert(!gxact->valid);
	gxact->valid = true;
	LWLockRelease(TwoPhaseStateLock);

	/*
	 * Put it into the global ProcArray so TransactionIdInProgress considers
	 * the XID as still running.
	 */
	ProcArrayAdd(&gxact->proc);
}

/*
 * LockGXact
 *		Locate the prepared transaction and mark it busy for COMMIT or PREPARE.
 */
static GlobalTransaction
LockGXact(char *gid, AclId user)
{
	int i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction	gxact = TwoPhaseState->prepXacts[i];

		/* Ignore not-yet-valid GIDs */
		if (!gxact->valid)
			continue;
		if (strcmp(gxact->gid, gid) != 0)
			continue;

		/* Found it, but has someone else got it locked? */
		if (TransactionIdIsValid(gxact->locking_xid))
		{
			if (TransactionIdIsActive(gxact->locking_xid))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("prepared transaction with gid \"%s\" is busy",
								gid)));
			gxact->locking_xid = InvalidTransactionId;
		}

		if (user != gxact->owner && !superuser_arg(user))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to finish prepared transaction"),
					 errhint("Must be superuser or the user that prepared the transaction.")));

		/* OK for me to lock it */
		gxact->locking_xid = GetTopTransactionId();

		LWLockRelease(TwoPhaseStateLock);

		return gxact;
	}

	LWLockRelease(TwoPhaseStateLock);

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("prepared transaction with gid \"%s\" does not exist",
					gid)));

	/* NOTREACHED */
	return NULL;
}

/*
 * RemoveGXact
 *		Remove the prepared transaction from the shared memory array.
 *
 * NB: caller should have already removed it from ProcArray
 */
static void
RemoveGXact(GlobalTransaction gxact)
{
	int i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		if (gxact == TwoPhaseState->prepXacts[i])
		{
			/* remove from the active array */
			TwoPhaseState->numPrepXacts--;
			TwoPhaseState->prepXacts[i] = TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts];

			/* and put it back in the freelist */
			gxact->proc.links.next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = MAKE_OFFSET(gxact);

			LWLockRelease(TwoPhaseStateLock);

			return;
		}
	}

	LWLockRelease(TwoPhaseStateLock);

	elog(ERROR, "failed to find %p in GlobalTransaction array", gxact);
}

/*
 * Returns an array of all prepared transactions for the user-level
 * function pg_prepared_xact.
 *
 * The returned array and all its elements are copies of internal data
 * structures, to minimize the time we need to hold the TwoPhaseStateLock.
 *
 * WARNING -- we return even those transactions that are not fully prepared
 * yet.  The caller should filter them out if he doesn't want them.
 *
 * The returned array is palloc'd.
 */
static int
GetPreparedTransactionList(GlobalTransaction *gxacts)
{
	GlobalTransaction array;
	int		num;
	int		i;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	if (TwoPhaseState->numPrepXacts == 0)
	{
		LWLockRelease(TwoPhaseStateLock);

		*gxacts = NULL;
		return 0;
	}

	num = TwoPhaseState->numPrepXacts;
	array = (GlobalTransaction) palloc(sizeof(GlobalTransactionData) * num);
	*gxacts = array;
	for (i = 0; i < num; i++)
		memcpy(array + i, TwoPhaseState->prepXacts[i],
			   sizeof(GlobalTransactionData));

	LWLockRelease(TwoPhaseStateLock);

	return num;
}


/* Working status for pg_prepared_xact */
typedef struct
{
	GlobalTransaction array;
	int		ngxacts;
	int		currIdx;
} Working_State;

/*
 * pg_prepared_xact
 * 		Produce a view with one row per prepared transaction.
 *
 * This function is here so we don't have to export the
 * GlobalTransactionData struct definition.
 */
Datum
pg_prepared_xact(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Working_State *status;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * Switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_prepared_xacts view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(4, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "transaction",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "gid",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "ownerid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "dbid",
						   OIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the 2PC status information that we will format and
		 * send out as a result set.
		 */
		status = (Working_State *) palloc(sizeof(Working_State));
		funcctx->user_fctx = (void *) status;

		status->ngxacts = GetPreparedTransactionList(&status->array);
		status->currIdx = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	status = (Working_State *) funcctx->user_fctx;

	while (status->array != NULL && status->currIdx < status->ngxacts)
	{
		GlobalTransaction gxact = &status->array[status->currIdx++];
		Datum		values[4];
		bool		nulls[4];
		HeapTuple	tuple;
		Datum		result;

		if (!gxact->valid)
			continue;

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[0] = TransactionIdGetDatum(gxact->proc.xid);
		values[1] = DirectFunctionCall1(textin, CStringGetDatum(gxact->gid));
		values[2] = Int32GetDatum(gxact->owner);
		values[3] = ObjectIdGetDatum(gxact->proc.databaseId);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * TwoPhaseGetDummyProc
 *		Get the PGPROC that represents a prepared transaction specified by XID
 */
PGPROC *
TwoPhaseGetDummyProc(TransactionId xid)
{
	PGPROC	   *result = NULL;
	int			i;

	static TransactionId cached_xid = InvalidTransactionId;
	static PGPROC *cached_proc = NULL;

	/*
	 * During a recovery, COMMIT PREPARED, or ABORT PREPARED, we'll be called
	 * repeatedly for the same XID.  We can save work with a simple cache.
	 */
	if (xid == cached_xid)
		return cached_proc;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction	gxact = TwoPhaseState->prepXacts[i];

		if (gxact->proc.xid == xid)
		{
			result = &gxact->proc;
			break;
		}
	}

	LWLockRelease(TwoPhaseStateLock);

	if (result == NULL)			/* should not happen */
		elog(ERROR, "failed to find dummy PGPROC for xid %u", xid);

	cached_xid = xid;
	cached_proc = result;

	return result;
}

/************************************************************************/
/* State file support                                                   */
/************************************************************************/

#define TwoPhaseFilePath(path, xid) \
	snprintf(path, MAXPGPATH, "%s/%s/%08X", DataDir, TWOPHASE_DIR, xid)

/*
 * 2PC state file format:
 *
 *  1. TwoPhaseFileHeader
 *  2. TransactionId[] (subtransactions)
 *	3. RelFileNode[] (files to be deleted at commit)
 *	4. RelFileNode[] (files to be deleted at abort)
 *  5. TwoPhaseRecordOnDisk
 *  6. ...
 *  7. TwoPhaseRecordOnDisk (end sentinel, rmid == TWOPHASE_RM_END_ID)
 *  8. CRC32
 *
 * Each segment except the final CRC32 is MAXALIGN'd.
 */

/*
 * Header for a 2PC state file
 */
#define TWOPHASE_MAGIC	0x57F94530		/* format identifier */

typedef struct TwoPhaseFileHeader
{
	uint32			magic;				/* format identifier */
	uint32			total_len;			/* actual file length */
	TransactionId	xid;				/* original transaction XID */
	Oid				database;			/* OID of database it was in */
	AclId			owner;				/* user running the transaction */
	int32			nsubxacts;			/* number of following subxact XIDs */
	int32			ncommitrels;		/* number of delete-on-commit rels */
	int32			nabortrels;			/* number of delete-on-abort rels */
	char			gid[GIDSIZE];		/* GID for transaction */
} TwoPhaseFileHeader;

/*
 * Header for each record in a state file
 *
 * NOTE: len counts only the rmgr data, not the TwoPhaseRecordOnDisk header.
 * The rmgr data will be stored starting on a MAXALIGN boundary.
 */
typedef struct TwoPhaseRecordOnDisk
{
	uint32			len;		/* length of rmgr data */
	TwoPhaseRmgrId	rmid;		/* resource manager for this record */
	uint16			info;		/* flag bits for use by rmgr */
} TwoPhaseRecordOnDisk;

/*
 * During prepare, the state file is assembled in memory before writing it
 * to WAL and the actual state file.  We use a chain of XLogRecData blocks
 * so that we will be able to pass the state file contents directly to
 * XLogInsert.
 */
static struct xllist
{
	XLogRecData *head;			/* first data block in the chain */
	XLogRecData *tail;			/* last block in chain */
	uint32 bytes_free;			/* free bytes left in tail block */
	uint32 total_len;			/* total data bytes in chain */
} records;


/*
 * Append a block of data to records data structure.
 *
 * NB: each block is padded to a MAXALIGN multiple.  This must be
 * accounted for when the file is later read!
 *
 * The data is copied, so the caller is free to modify it afterwards.
 */
static void
save_state_data(const void *data, uint32 len)
{
	uint32	padlen = MAXALIGN(len);

	if (padlen > records.bytes_free)
	{
		records.tail->next = palloc0(sizeof(XLogRecData));
		records.tail = records.tail->next;
		records.tail->buffer = InvalidBuffer;
		records.tail->len = 0;
		records.tail->next = NULL;

		records.bytes_free = Max(padlen, 512);
		records.tail->data = palloc(records.bytes_free);
	}

	memcpy(((char *) records.tail->data) + records.tail->len, data, len);
	records.tail->len += padlen;
	records.bytes_free -= padlen;
	records.total_len += padlen;
}

/*
 * Start preparing a state file.
 *
 * Initializes data structure and inserts the 2PC file header record.
 */
void
StartPrepare(GlobalTransaction gxact)
{
	TransactionId	xid = gxact->proc.xid;
	TwoPhaseFileHeader hdr;
	TransactionId *children;
	RelFileNode *commitrels;
	RelFileNode *abortrels;

	/* Initialize linked list */
	records.head = palloc0(sizeof(XLogRecData));
	records.head->buffer = InvalidBuffer;
	records.head->len = 0;
	records.head->next = NULL;

	records.bytes_free = Max(sizeof(TwoPhaseFileHeader), 512);
	records.head->data = palloc(records.bytes_free);

	records.tail = records.head;

	records.total_len = 0;

	/* Create header */
	hdr.magic = TWOPHASE_MAGIC;
	hdr.total_len = 0;			/* EndPrepare will fill this in */
	hdr.xid = xid;
	hdr.database = MyDatabaseId;
	hdr.owner = GetUserId();
	hdr.nsubxacts = xactGetCommittedChildren(&children);
	hdr.ncommitrels = smgrGetPendingDeletes(true, &commitrels);
	hdr.nabortrels = smgrGetPendingDeletes(false, &abortrels);
	StrNCpy(hdr.gid, gxact->gid, GIDSIZE);

	save_state_data(&hdr, sizeof(TwoPhaseFileHeader));

	/* Add the additional info about subxacts and deletable files */
	if (hdr.nsubxacts > 0)
	{
		save_state_data(children, hdr.nsubxacts * sizeof(TransactionId));
		/* While we have the child-xact data, stuff it in the gxact too */
		GXactLoadSubxactData(gxact, hdr.nsubxacts, children);
		pfree(children);
	}
	if (hdr.ncommitrels > 0)
	{
		save_state_data(commitrels, hdr.ncommitrels * sizeof(RelFileNode));
		pfree(commitrels);
	}
	if (hdr.nabortrels > 0)
	{
		save_state_data(abortrels, hdr.nabortrels * sizeof(RelFileNode));
		pfree(abortrels);
	}
}

/*
 * Finish preparing state file.
 *
 * Calculates CRC and writes state file to WAL and in pg_twophase directory.
 */
void
EndPrepare(GlobalTransaction gxact)
{
	TransactionId	xid = gxact->proc.xid;
	TwoPhaseFileHeader *hdr;
	char			path[MAXPGPATH];
	XLogRecData	   *record;
	XLogRecPtr		recptr;
	pg_crc32		statefile_crc;
	pg_crc32		bogus_crc;
	int				fd;

	/* Add the end sentinel to the list of 2PC records */
	RegisterTwoPhaseRecord(TWOPHASE_RM_END_ID, 0,
						   NULL, 0);

	/* Go back and fill in total_len in the file header record */
	hdr = (TwoPhaseFileHeader *) records.head->data;
	Assert(hdr->magic == TWOPHASE_MAGIC);
	hdr->total_len = records.total_len + sizeof(pg_crc32);

	/*
	 * Create the 2PC state file.
	 *
	 * Note: because we use BasicOpenFile(), we are responsible for ensuring
	 * the FD gets closed in any error exit path.  Once we get into the
	 * critical section, though, it doesn't matter since any failure causes
	 * PANIC anyway.
	 */
	TwoPhaseFilePath(path, xid);

	fd = BasicOpenFile(path,
					   O_CREAT | O_EXCL | O_WRONLY | PG_BINARY,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create twophase state file \"%s\": %m",
						path)));

	/* Write data to file, and calculate CRC as we pass over it */
	INIT_CRC32(statefile_crc);

	for (record = records.head; record != NULL; record = record->next)
	{
		COMP_CRC32(statefile_crc, record->data, record->len);
		if ((write(fd, record->data, record->len)) != record->len)
		{
			close(fd);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write twophase state file: %m")));
		}
	}

	FIN_CRC32(statefile_crc);

	/*
	 * Write a deliberately bogus CRC to the state file, and flush it to disk.
	 * This is to minimize the odds of failure within the critical section
	 * below --- in particular, running out of disk space.
	 *
	 * On most filesystems, write() rather than fsync() detects out-of-space,
	 * so the fsync might be considered optional.  Using it means there
	 * are three fsyncs not two associated with preparing a transaction; is
	 * the risk of an error from fsync high enough to justify that?
	 */
	bogus_crc = ~ statefile_crc;

	if ((write(fd, &bogus_crc, sizeof(pg_crc32))) != sizeof(pg_crc32))
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write twophase state file: %m")));
	}

	if (pg_fsync(fd) != 0)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync twophase state file: %m")));
	}

	/* Back up to prepare for rewriting the CRC */
	if (lseek(fd, -((off_t) sizeof(pg_crc32)), SEEK_CUR) < 0)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek twophase state file: %m")));
	}

	/*
	 * The state file isn't valid yet, because we haven't written the correct
	 * CRC yet.  Before we do that, insert entry in WAL and flush it to disk.
	 *
	 * Between the time we have written the WAL entry and the time we
	 * flush the correct state file CRC to disk, we have an inconsistency:
	 * the xact is prepared according to WAL but not according to our on-disk
	 * state.  We use a critical section to force a PANIC if we are unable to
	 * complete the flush --- then, WAL replay should repair the
	 * inconsistency.
	 *
	 * We have to lock out checkpoint start here, too; otherwise a checkpoint
	 * starting immediately after the WAL record is inserted could complete
	 * before we've finished flushing, meaning that the WAL record would not
	 * get replayed if a crash follows.
	 */
	START_CRIT_SECTION();

	LWLockAcquire(CheckpointStartLock, LW_SHARED);

	recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_PREPARE, records.head);
	XLogFlush(recptr);

	/* If we crash now, we have prepared: WAL replay will fix things */

	/* write correct CRC, flush, and close file */
	if ((write(fd, &statefile_crc, sizeof(pg_crc32))) != sizeof(pg_crc32))
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write twophase state file: %m")));
	}

	if (pg_fsync(fd) != 0)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync twophase state file: %m")));
	}

	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close twophase state file: %m")));

	LWLockRelease(CheckpointStartLock);

	END_CRIT_SECTION();

	records.tail = records.head = NULL;
}

/*
 * Register a 2PC record to be written to state file.
 */
void
RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, uint16 info,
					   const void *data, uint32 len)
{
	TwoPhaseRecordOnDisk record;

	record.rmid = rmid;
	record.info = info;
	record.len = len;
	save_state_data(&record, sizeof(TwoPhaseRecordOnDisk));
	if (len > 0)
		save_state_data(data, len);
}


/*
 * Read and validate the state file for xid.
 *
 * If it looks OK (has a valid magic number and CRC), return the palloc'd
 * contents of the file.  Otherwise return NULL.
 */
static char *
ReadTwoPhaseFile(TransactionId xid)
{
	char		path[MAXPGPATH];
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	int			fd;
	struct stat	stat;
	uint32		crc_offset;
	pg_crc32	calc_crc, file_crc;

	TwoPhaseFilePath(path, xid);

	fd = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open twophase state file \"%s\": %m",
						path)));
		return NULL;
	}

	/*
	 * Check file length.  We can determine a lower bound pretty easily.
	 * We set an upper bound mainly to avoid palloc() failure on a corrupt
	 * file.
	 */
	if (fstat(fd, &stat))
	{
		close(fd);
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not stat twophase state file \"%s\": %m",
						path)));
		return NULL;
	}

	if (stat.st_size < (MAXALIGN(sizeof(TwoPhaseFileHeader)) +
						MAXALIGN(sizeof(TwoPhaseRecordOnDisk)) +
						sizeof(pg_crc32)) ||
		stat.st_size > 10000000)
	{
		close(fd);
		return NULL;
	}

	crc_offset = stat.st_size - sizeof(pg_crc32);
	if (crc_offset != MAXALIGN(crc_offset))
	{
		close(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.
	 */
	buf = (char *) palloc(stat.st_size);

	if (read(fd, buf, stat.st_size) != stat.st_size)
	{
		close(fd);
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not read twophase state file \"%s\": %m",
						path)));
		pfree(buf);
		return NULL;
	}

	close(fd);

	hdr = (TwoPhaseFileHeader *) buf;
	if (hdr->magic != TWOPHASE_MAGIC || hdr->total_len != stat.st_size)
	{
		pfree(buf);
		return NULL;
	}

	INIT_CRC32(calc_crc);
	COMP_CRC32(calc_crc, buf, crc_offset);
	FIN_CRC32(calc_crc);

	file_crc = *((pg_crc32 *) (buf + crc_offset));

	if (!EQ_CRC32(calc_crc, file_crc))
	{
		pfree(buf);
		return NULL;
	}

	return buf;
}


/*
 * FinishPreparedTransaction: execute COMMIT PREPARED or ROLLBACK PREPARED
 */
void
FinishPreparedTransaction(char *gid, bool isCommit)
{
	GlobalTransaction gxact;
	TransactionId xid;
	char *buf;
	char *bufptr;
	TwoPhaseFileHeader *hdr;
	TransactionId *children;
	RelFileNode *commitrels;
	RelFileNode *abortrels;
	int		i;

	/*
	 * Validate the GID, and lock the GXACT to ensure that two backends
	 * do not try to commit the same GID at once.
	 */
	gxact = LockGXact(gid, GetUserId());
	xid = gxact->proc.xid;

	/*
	 * Read and validate the state file
	 */
	buf = ReadTwoPhaseFile(xid);
	if (buf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("twophase state file for transaction %u is corrupt",
						xid)));

	/*
	 * Disassemble the header area
	 */
	hdr = (TwoPhaseFileHeader *) buf;
	Assert(TransactionIdEquals(hdr->xid, xid));
	bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
	children = (TransactionId *) bufptr;
	bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
	commitrels = (RelFileNode *) bufptr;
	bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileNode));
	abortrels = (RelFileNode *) bufptr;
	bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileNode));

	/*
	 * The order of operations here is critical: make the XLOG entry for
	 * commit or abort, then mark the transaction committed or aborted in
	 * pg_clog, then remove its PGPROC from the global ProcArray (which
	 * means TransactionIdIsInProgress will stop saying the prepared xact
	 * is in progress), then run the post-commit or post-abort callbacks.
	 * The callbacks will release the locks the transaction held.
	 */
	if (isCommit)
		RecordTransactionCommitPrepared(xid,
										hdr->nsubxacts, children,
										hdr->ncommitrels, commitrels);
	else
		RecordTransactionAbortPrepared(xid,
									   hdr->nsubxacts, children,
									   hdr->nabortrels, abortrels);

	ProcArrayRemove(&gxact->proc);

	/*
	 * In case we fail while running the callbacks, mark the gxact invalid
	 * so no one else will try to commit/rollback, and so it can be recycled
	 * properly later.  It is still locked by our XID so it won't go away yet.
	 */
	gxact->valid = false;

	if (isCommit)
		ProcessRecords(bufptr, xid, twophase_postcommit_callbacks);
	else
		ProcessRecords(bufptr, xid, twophase_postabort_callbacks);

	/*
	 * We also have to remove any files that were supposed to be dropped.
	 * NB: this code knows that we couldn't be dropping any temp rels ...
	 */
	if (isCommit)
	{
		for (i = 0; i < hdr->ncommitrels; i++)
			smgrdounlink(smgropen(commitrels[i]), false, false);
	}
	else
	{
		for (i = 0; i < hdr->nabortrels; i++)
			smgrdounlink(smgropen(abortrels[i]), false, false);
	}

	pgstat_count_xact_commit();

	/*
	 * And now we can clean up our mess.
	 */
	RemoveTwoPhaseFile(xid, true);

	RemoveGXact(gxact);

	pfree(buf);
}

/*
 * Scan a 2PC state file (already read into memory by ReadTwoPhaseFile)
 * and call the indicated callbacks for each 2PC record.
 */
static void
ProcessRecords(char *bufptr, TransactionId xid,
			   const TwoPhaseCallback callbacks[])
{
	for (;;)
	{
		TwoPhaseRecordOnDisk *record = (TwoPhaseRecordOnDisk *) bufptr;

		Assert(record->rmid <= TWOPHASE_RM_MAX_ID);
		if (record->rmid == TWOPHASE_RM_END_ID)
			break;

		bufptr += MAXALIGN(sizeof(TwoPhaseRecordOnDisk));

		if (callbacks[record->rmid] != NULL)
			callbacks[record->rmid](xid, record->info,
									(void *) bufptr, record->len);

		bufptr += MAXALIGN(record->len);
	}
}

/*
 * Remove the 2PC file for the specified XID.
 *
 * If giveWarning is false, do not complain about file-not-present;
 * this is an expected case during WAL replay.
 */
void
RemoveTwoPhaseFile(TransactionId xid, bool giveWarning)
{
	char path[MAXPGPATH];

	TwoPhaseFilePath(path, xid);
	if (unlink(path))
		if (errno != ENOENT || giveWarning)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove two-phase state file \"%s\": %m",
							path)));
}

/*
 * Recreates a state file. This is used in WAL replay.
 *
 * Note: content and len don't include CRC.
 */
void
RecreateTwoPhaseFile(TransactionId xid, void *content, int len)
{
	char		path[MAXPGPATH];
	pg_crc32	statefile_crc;
	int			fd;

	/* Recompute CRC */
	INIT_CRC32(statefile_crc);
	COMP_CRC32(statefile_crc, content, len);
	FIN_CRC32(statefile_crc);

	TwoPhaseFilePath(path, xid);

	fd = BasicOpenFile(path,
					   O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recreate twophase state file \"%s\": %m",
						path)));

	/* Write content and CRC */
	if (write(fd, content, len) != len)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write twophase state file: %m")));
	}
	if (write(fd, &statefile_crc, sizeof(pg_crc32)) != sizeof(pg_crc32))
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write twophase state file: %m")));
	}

	/* Sync and close the file */
	if (pg_fsync(fd) != 0)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync twophase state file: %m")));
	}

	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close twophase state file: %m")));
}

/*
 * PrescanPreparedTransactions
 *
 * Scan the pg_twophase directory and determine the range of valid XIDs
 * present.  This is run during database startup, after we have completed
 * reading WAL.  ShmemVariableCache->nextXid has been set to one more than
 * the highest XID for which evidence exists in WAL.
 *
 * We throw away any prepared xacts with main XID beyond nextXid --- if any
 * are present, it suggests that the DBA has done a PITR recovery to an
 * earlier point in time without cleaning out pg_twophase.  We dare not
 * try to recover such prepared xacts since they likely depend on database
 * state that doesn't exist now.
 *
 * However, we will advance nextXid beyond any subxact XIDs belonging to
 * valid prepared xacts.  We need to do this since subxact commit doesn't
 * write a WAL entry, and so there might be no evidence in WAL of those
 * subxact XIDs.
 *
 * Our other responsibility is to determine and return the oldest valid XID
 * among the prepared xacts (if none, return ShmemVariableCache->nextXid).
 * This is needed to synchronize pg_subtrans startup properly.
 */
TransactionId
PrescanPreparedTransactions(void)
{
	TransactionId origNextXid = ShmemVariableCache->nextXid;
	TransactionId result = origNextXid;
	char	dir[MAXPGPATH];
	DIR		*cldir;
	struct dirent *clde;

	snprintf(dir, MAXPGPATH, "%s/%s", DataDir, TWOPHASE_DIR);

	cldir = AllocateDir(dir);
	if (cldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", dir)));

	errno = 0;
	while ((clde = readdir(cldir)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char *buf;
			TwoPhaseFileHeader	*hdr;
			TransactionId *subxids;
			int i;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			/* Reject XID if too new */
			if (TransactionIdFollowsOrEquals(xid, origNextXid))
			{
				ereport(WARNING,
						(errmsg("removing future twophase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				errno = 0;
				continue;
			}

			/*
			 * Note: we can't check if already processed because clog
			 * subsystem isn't up yet.
			 */

			/* Read and validate file */
			buf = ReadTwoPhaseFile(xid);
			if (buf == NULL)
			{
				ereport(WARNING,
						(errmsg("removing corrupt twophase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				errno = 0;
				continue;
			}

			/* Deconstruct header */
			hdr = (TwoPhaseFileHeader *) buf;
			if (!TransactionIdEquals(hdr->xid, xid))
			{
				ereport(WARNING,
						(errmsg("removing corrupt twophase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				pfree(buf);
				errno = 0;
				continue;
			}

			/*
			 * OK, we think this file is valid.  Incorporate xid into the
			 * running-minimum result.
			 */
			if (TransactionIdPrecedes(xid, result))
				result = xid;

			/*
			 * Examine subtransaction XIDs ... they should all follow main
			 * XID, and they may force us to advance nextXid.
			 */
			subxids = (TransactionId *)
				(buf + MAXALIGN(sizeof(TwoPhaseFileHeader)));
			for (i = 0; i < hdr->nsubxacts; i++)
			{
				TransactionId subxid = subxids[i];

				Assert(TransactionIdFollows(subxid, xid));
				if (TransactionIdFollowsOrEquals(subxid,
												 ShmemVariableCache->nextXid))
				{
					ShmemVariableCache->nextXid = subxid;
					TransactionIdAdvance(ShmemVariableCache->nextXid);
				}
			}

			pfree(buf);
		}
		errno = 0;
	}
#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but
	 * not in released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif
	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m", dir)));

	FreeDir(cldir);

	return result;
}

/*
 * RecoverPreparedTransactions
 *
 * Scan the pg_twophase directory and reload shared-memory state for each
 * prepared transaction (reacquire locks, etc).  This is run during database
 * startup.
 */
void
RecoverPreparedTransactions(void)
{
	char	dir[MAXPGPATH];
	DIR		*cldir;
	struct dirent *clde;

	snprintf(dir, MAXPGPATH, "%s/%s", DataDir, TWOPHASE_DIR);

	cldir = AllocateDir(dir);
	if (cldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", dir)));

	errno = 0;
	while ((clde = readdir(cldir)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char *buf;
			char *bufptr;
			TwoPhaseFileHeader	*hdr;
			TransactionId *subxids;
			GlobalTransaction	gxact;
			int i;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			/* Already processed? */
			if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			{
				ereport(WARNING,
						(errmsg("removing stale twophase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				errno = 0;
				continue;
			}

			/* Read and validate file */
			buf = ReadTwoPhaseFile(xid);
			if (buf == NULL)
			{
				ereport(WARNING,
						(errmsg("removing corrupt twophase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				errno = 0;
				continue;
			}

			ereport(LOG,
					(errmsg("recovering prepared transaction %u", xid)));

			/* Deconstruct header */
			hdr = (TwoPhaseFileHeader *) buf;
			Assert(TransactionIdEquals(hdr->xid, xid));
			bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
			subxids = (TransactionId *) bufptr;
			bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
			bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileNode));
			bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileNode));

			/*
			 * Reconstruct subtrans state for the transaction --- needed
			 * because pg_subtrans is not preserved over a restart
			 */
			for (i = 0; i < hdr->nsubxacts; i++)
				SubTransSetParent(subxids[i], xid);

			/*
			 * Recreate its GXACT and dummy PGPROC
			 */
			gxact = MarkAsPreparing(xid, hdr->database, hdr->gid, hdr->owner);
			GXactLoadSubxactData(gxact, hdr->nsubxacts, subxids);
			MarkAsPrepared(gxact);

			/*
			 * Recover other state (notably locks) using resource managers
			 */
			ProcessRecords(bufptr, xid, twophase_recover_callbacks);

			pfree(buf);
		}
		errno = 0;
	}
#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but
	 * not in released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif
	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m", dir)));

	FreeDir(cldir);
}

/*
 *	RecordTransactionCommitPrepared
 *
 * This is basically the same as RecordTransactionCommit: in particular,
 * we must take the CheckpointStartLock to avoid a race condition.
 *
 * We know the transaction made at least one XLOG entry (its PREPARE),
 * so it is never possible to optimize out the commit record.
 */
static void
RecordTransactionCommitPrepared(TransactionId xid,
								int nchildren,
								TransactionId *children,
								int nrels,
								RelFileNode *rels)
{
	XLogRecData rdata[3];
	int			lastrdata = 0;
	xl_xact_commit_prepared xlrec;
	XLogRecPtr	recptr;

	START_CRIT_SECTION();

	/* See notes in RecordTransactionCommit */
	LWLockAcquire(CheckpointStartLock, LW_SHARED);

	/* Emit the XLOG commit record */
	xlrec.xid = xid;
	xlrec.crec.xtime = time(NULL);
	xlrec.crec.nrels = nrels;
	xlrec.crec.nsubxacts = nchildren;
	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfXactCommitPrepared;
	rdata[0].buffer = InvalidBuffer;
	/* dump rels to delete */
	if (nrels > 0)
	{
		rdata[0].next = &(rdata[1]);
		rdata[1].data = (char *) rels;
		rdata[1].len = nrels * sizeof(RelFileNode);
		rdata[1].buffer = InvalidBuffer;
		lastrdata = 1;
	}
	/* dump committed child Xids */
	if (nchildren > 0)
	{
		rdata[lastrdata].next = &(rdata[2]);
		rdata[2].data = (char *) children;
		rdata[2].len = nchildren * sizeof(TransactionId);
		rdata[2].buffer = InvalidBuffer;
		lastrdata = 2;
	}
	rdata[lastrdata].next = NULL;

	recptr = XLogInsert(RM_XACT_ID,
						XLOG_XACT_COMMIT_PREPARED | XLOG_NO_TRAN,
						rdata);

	/* we don't currently try to sleep before flush here ... */

	/* Flush XLOG to disk */
	XLogFlush(recptr);

	/* Mark the transaction committed in pg_clog */
	TransactionIdCommit(xid);
	/* to avoid race conditions, the parent must commit first */
	TransactionIdCommitTree(nchildren, children);

	/* Checkpoint is allowed again */
	LWLockRelease(CheckpointStartLock);

	END_CRIT_SECTION();
}

/*
 *	RecordTransactionAbortPrepared
 *
 * This is basically the same as RecordTransactionAbort.
 *
 * We know the transaction made at least one XLOG entry (its PREPARE),
 * so it is never possible to optimize out the abort record.
 */
static void
RecordTransactionAbortPrepared(TransactionId xid,
							   int nchildren,
							   TransactionId *children,
							   int nrels,
							   RelFileNode *rels)
{
	XLogRecData rdata[3];
	int			lastrdata = 0;
	xl_xact_abort_prepared xlrec;
	XLogRecPtr	recptr;

	/*
	 * Catch the scenario where we aborted partway through
	 * RecordTransactionCommitPrepared ...
	 */
	if (TransactionIdDidCommit(xid))
		elog(PANIC, "cannot abort transaction %u, it was already committed",
			 xid);

	START_CRIT_SECTION();

	/* Emit the XLOG abort record */
	xlrec.xid = xid;
	xlrec.arec.xtime = time(NULL);
	xlrec.arec.nrels = nrels;
	xlrec.arec.nsubxacts = nchildren;
	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfXactAbortPrepared;
	rdata[0].buffer = InvalidBuffer;
	/* dump rels to delete */
	if (nrels > 0)
	{
		rdata[0].next = &(rdata[1]);
		rdata[1].data = (char *) rels;
		rdata[1].len = nrels * sizeof(RelFileNode);
		rdata[1].buffer = InvalidBuffer;
		lastrdata = 1;
	}
	/* dump committed child Xids */
	if (nchildren > 0)
	{
		rdata[lastrdata].next = &(rdata[2]);
		rdata[2].data = (char *) children;
		rdata[2].len = nchildren * sizeof(TransactionId);
		rdata[2].buffer = InvalidBuffer;
		lastrdata = 2;
	}
	rdata[lastrdata].next = NULL;

	recptr = XLogInsert(RM_XACT_ID,
						XLOG_XACT_ABORT_PREPARED | XLOG_NO_TRAN,
						rdata);

	/* Always flush, since we're about to remove the 2PC state file */
	XLogFlush(recptr);

	/*
	 * Mark the transaction aborted in clog.  This is not absolutely
	 * necessary but we may as well do it while we are here.
	 */
	TransactionIdAbort(xid);
	TransactionIdAbortTree(nchildren, children);

	END_CRIT_SECTION();
}
