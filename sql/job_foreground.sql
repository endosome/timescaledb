-- This file and its contents are licensed under the Apache License 2.0.
-- Please see the included NOTICE for copyright information and
-- LICENSE-APACHE for a copy of the license.

-- Running scheduled jobs in the foreground, for hosts without background
-- workers. See src/bgw/foreground.c.

CREATE OR REPLACE FUNCTION _timescaledb_functions.job_next_start(job_id INTEGER)
RETURNS TIMESTAMPTZ
AS '@MODULE_PATHNAME@', 'ts_bgw_job_next_start'
LANGUAGE C VOLATILE STRICT;

-- The scheduled jobs, earliest first, with when each is due to run next.
CREATE OR REPLACE FUNCTION _timescaledb_functions.job_schedule()
RETURNS TABLE (job_id INTEGER, next_start TIMESTAMPTZ, due BOOLEAN)
AS $$
    SELECT j.id, n.next_start, n.next_start <= clock_timestamp()
    FROM _timescaledb_catalog.bgw_job j,
    LATERAL _timescaledb_functions.job_next_start(j.id) AS n(next_start)
    WHERE j.scheduled
    ORDER BY n.next_start, j.id
$$ LANGUAGE SQL VOLATILE SET search_path TO pg_catalog, pg_temp;

CREATE OR REPLACE PROCEDURE _timescaledb_functions.job_run_with_stats(job_id INTEGER)
AS '@MODULE_PATHNAME@', 'ts_bgw_job_run_with_stats'
LANGUAGE C;

CREATE OR REPLACE FUNCTION _timescaledb_functions.job_mark_failed(
    job_id INTEGER,
    sqlstate TEXT,
    message TEXT,
    detail TEXT = NULL,
    hint TEXT = NULL
) RETURNS VOID
AS '@MODULE_PATHNAME@', 'ts_bgw_job_mark_failed'
LANGUAGE C VOLATILE;
