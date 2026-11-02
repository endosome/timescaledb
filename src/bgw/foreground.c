/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */

/*
 * Running scheduled jobs in the foreground.
 *
 * Where background workers are not available (e.g. PGlite, which runs a single
 * backend inside WebAssembly) the host application drives the job schedule
 * itself:
 *
 *   SELECT job_id FROM _timescaledb_functions.job_schedule() WHERE due;
 *   CALL _timescaledb_functions.job_run_with_stats(job_id);
 *   -- on error:
 *   SELECT _timescaledb_functions.job_mark_failed(job_id, sqlstate, message, detail, hint);
 *
 * These keep the same bookkeeping as the background worker scheduler: job
 * statistics, next_start (including fixed schedules and retry backoff on
 * failures and crashes) and the job history/error tables.
 */
#include <postgres.h>
#include <access/xact.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <nodes/parsenodes.h>
#include <tcop/pquery.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>

#include "bgw/job.h"
#include "bgw/job_stat.h"
#include "bgw/job_stat_history.h"
#include "bgw/timer.h"
#include "export.h"
#include "guc.h"
#include "utils.h"

TS_FUNCTION_INFO_V1(ts_bgw_job_next_start);
TS_FUNCTION_INFO_V1(ts_bgw_job_run_with_stats);
TS_FUNCTION_INFO_V1(ts_bgw_job_mark_failed);

/*
 * _timescaledb_functions.job_next_start(job_id INTEGER) RETURNS TIMESTAMPTZ
 *
 * When the scheduler would start the job next. A job that has never run is
 * due right away. If the last run never finished (the process running it
 * died), the crash is recorded and the retry backoff for crashes applies.
 */
Datum
ts_bgw_job_next_start(PG_FUNCTION_ARGS)
{
	int32 job_id = PG_GETARG_INT32(0);
	BgwJob *job = ts_bgw_job_find(job_id, CurrentMemoryContext, true);
	BgwJobStat *job_stat = ts_bgw_job_stat_find(job_id);
	TimestampTz next_start = ts_bgw_job_stat_next_start(job_stat, job);

	if (TIMESTAMP_IS_NOBEGIN(next_start))
		next_start = ts_timer_get_current_timestamp();

	PG_RETURN_TIMESTAMPTZ(next_start);
}

/*
 * CALL _timescaledb_functions.job_run_with_stats(job_id INTEGER)
 *
 * Like run_job(), but records the run in the job statistics like the
 * background worker scheduler does. The start of the run is committed before
 * the job executes, so a run that never finishes is detected as a crash by
 * job_next_start(). If the job raises an error, the caller has to record the
 * failure with job_mark_failed(): the error aborts this transaction, and
 * catching it here would need a subtransaction, which would prevent the job
 * from committing its own transactions.
 */
Datum
ts_bgw_job_run_with_stats(PG_FUNCTION_ARGS)
{
	int32 job_id = PG_GETARG_INT32(0);
	bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
					 !castNode(CallContext, fcinfo->context)->atomic;
	/* the job has to survive the commit below */
	MemoryContext job_ctx = AllocSetContextCreate(TopMemoryContext,
												  "job_run_with_stats",
												  ALLOCSET_SMALL_SIZES);
	JobResult res;

	PreventInTransactionBlock(nonatomic, "job_run_with_stats()");

	PG_TRY();
	{
		BgwJob *job = ts_bgw_job_find(job_id, job_ctx, true);

		ts_bgw_job_permission_check(job, "run");

		ts_bgw_job_stat_mark_start(job);

		SPI_connect_ext(SPI_OPT_NONATOMIC);
		SPI_commit();
		SPI_finish();
		EnsurePortalSnapshotExists();

		res = ts_bgw_job_execute(job);

		if (!IsTransactionState())
			StartTransactionCommand();
		EnsurePortalSnapshotExists();

		ts_bgw_job_stat_mark_end(job, res, NULL);
	}
	PG_FINALLY();
	{
		MemoryContextDelete(job_ctx);
	}
	PG_END_TRY();

	PG_RETURN_VOID();
}

/*
 * The history entry opened by the start of the job's current run, if job
 * execution logging is enabled.
 */
static int64
open_history_entry(int32 job_id)
{
	int64 history_id = INVALID_BGW_JOB_STAT_HISTORY_ID;
	Oid argtypes[] = { INT4OID };
	Datum args[] = { Int32GetDatum(job_id) };
	bool isnull;

	if (!ts_guc_enable_job_execution_logging)
		return history_id;

	SPI_connect();
	if (SPI_execute_with_args("SELECT id FROM _timescaledb_internal.bgw_job_stat_history "
							  "WHERE job_id = $1 AND execution_finish IS NULL "
							  "ORDER BY id DESC LIMIT 1",
							  1,
							  argtypes,
							  args,
							  NULL,
							  true,
							  1) == SPI_OK_SELECT &&
		SPI_processed == 1)
	{
		Datum id = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull)
			history_id = DatumGetInt64(id);
	}
	SPI_finish();

	return history_id;
}

/*
 * _timescaledb_functions.job_mark_failed(job_id INTEGER, sqlstate TEXT, message TEXT,
 *                                        detail TEXT, hint TEXT) RETURNS VOID
 *
 * Records the failure of the job's current run (started by
 * job_run_with_stats()) with the error it raised: updates the job statistics,
 * sets next_start according to the retry period, and logs the error.
 */
Datum
ts_bgw_job_mark_failed(PG_FUNCTION_ARGS)
{
	int32 job_id = PG_GETARG_INT32(0);
	BgwJob *job = ts_bgw_job_find(job_id, CurrentMemoryContext, true);
	BgwJobStat *job_stat;
	ErrorData *edata = palloc0(sizeof(ErrorData));

	ts_bgw_job_permission_check(job, "run");

	job_stat = ts_bgw_job_stat_find(job_id);
	if (job_stat == NULL || ts_bgw_job_stat_end_was_marked(job_stat))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("job %d has no unfinished run", job_id)));

	edata->elevel = ERROR;
	edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
	if (!PG_ARGISNULL(1))
	{
		char *sqlstate = text_to_cstring(PG_GETARG_TEXT_PP(1));

		if (strlen(sqlstate) == 5)
			edata->sqlerrcode =
				MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2], sqlstate[3], sqlstate[4]);
	}
	edata->message = PG_ARGISNULL(2) ? "failed to execute job" :
									   text_to_cstring(PG_GETARG_TEXT_PP(2));
	edata->detail = PG_ARGISNULL(3) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(3));
	edata->hint = PG_ARGISNULL(4) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(4));

	job->job_history.id = open_history_entry(job_id);
	job->job_history.execution_start = job_stat->fd.last_start;

	ts_bgw_job_stat_mark_end(job,
							 JOB_FAILURE_IN_EXECUTION,
							 ts_errdata_to_jsonb(edata, &job->fd.proc_schema, &job->fd.proc_name));

	PG_RETURN_VOID();
}
