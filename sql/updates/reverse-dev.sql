DROP FUNCTION IF EXISTS _timescaledb_functions.job_schedule();
DROP FUNCTION IF EXISTS _timescaledb_functions.job_next_start(INTEGER);
DROP PROCEDURE IF EXISTS _timescaledb_functions.job_run_with_stats(INTEGER);
DROP FUNCTION IF EXISTS _timescaledb_functions.job_mark_failed(INTEGER, TEXT, TEXT, TEXT, TEXT);
