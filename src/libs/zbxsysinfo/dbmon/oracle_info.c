/*
** Zabbix
** Copyright (C) 2019-2022 Mikhail Grigorev
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "sysinfo.h"
#include "log.h"
#include "module.h"
#include "zbxdbmon.h"
#include "oracle_info.h"
#include "dbmon_common.h"
#include "dbmon_config.h"
#include "dbmon_params.h"

#if defined(HAVE_DBMON)
#if defined(HAVE_ORACLE)

extern int init_dbmon_config_done;

extern char	*CONFIG_ORACLE_USER;
extern char	*CONFIG_ORACLE_PASSWORD;
extern char	*CONFIG_ORACLE_INSTANCE;
extern char	*CONFIG_ORACLE_PRIMARY_USER;
extern char	*CONFIG_ORACLE_PRIMARY_PASSWORD;
extern int CONFIG_ORACLE_USE_LOCAL_ENV;

#define ORACLE_DEFAULT_USER	"sys"
#define ORACLE_DEFAULT_PASSWORD	"sys"
#define ORACLE_DEFAULT_INSTANCE	"orcl"

#define ORACLE_CHECK_SUSPEND_MODE_DBS "\
SELECT DECODE(database_status, 'ACTIVE', 1, 'SUSPENDED', 2, 'INSTANCE RECOVERY', 3, 0) DBSTATUS \
FROM v$instance \
WHERE instance_name = '%s'"

/*#define ORACLE_CHECK_ARCHIVELOG_MODE_DBS "\
SELECT DECODE(archiver, 'STOPPED', 1, 'STARTED', 2, 'FAILED', 3, 0) ARCHIVER \
FROM v$instance \
WHERE instance_name = '%s'"*/

#define ORACLE_CHECK_ARCHIVELOG_MODE_DBS "\
SELECT DECODE(d.log_mode,'NOARCHIVELOG',1,'ARCHIVELOG',2,'MANUAL',3,0) log_mode \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id AND i.instance_name = '%s' AND d.name = '%s'"

#define ORACLE_VERSION_DBS "\
SELECT version AS VERSION \
FROM v$instance \
WHERE instance_name = '%s'"

#define ORACLE_INSTANCE_INFO_DBS "\
SELECT host_name AS HOSTNAME, \
	TO_CHAR(round(((sysdate - startup_time) * 60 * 60 * 24), 0)) AS UPTIME, \
	DECODE(status, 'STARTED', 1, 'MOUNTED', 2, 'OPEN', 3, 'OPEN MIGRATE', 4, 0) AS STATUS, \
	DECODE(parallel, 'YES', 1, 'NO', 2, 0) PARALLEL, \
	DECODE(archiver, 'STOPPED', 1, 'STARTED', 2, 'FAILED', 3, 0) ARCHIVER, \
	DECODE(database_status, 'ACTIVE', 1, 'SUSPENDED', 2, 'INSTANCE RECOVERY', 3, 0) DBSTATUS, \
	DECODE(active_state, 'NORMAL', 1, 'QUIESCING', 2, 'QUIESCED', 3, 0) AS ACTIVESTATE \
FROM v$instance \
WHERE instance_name = '%s'"

#define ORACLE_V11_DISCOVERY_DB_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
    d.name AS DBNAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND i.instance_name= '%s'"

#define ORACLE_V12_DISCOVERY_DB_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
    d.name AS DBNAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND i.instance_name= '%s'"

#define ORACLE_V11_DISCOVERY_PDB_DBS "\
SELECT i.instance_name AS INSTANCE, \
	i.host_name AS HOSTNAME, \
	d.name AS DBNAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND i.instance_name = 'FAKEINSTANCENAME'"

#define ORACLE_V12_DISCOVERY_PDB_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
    pd.name AS PDBNAME \
FROM gv$instance i, gv$pdbs pd \
WHERE i.inst_id = pd.inst_id \
	AND i.instance_name= '%s'"

#define ORACLE_V11_DB_INFO_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
	DECODE(d.log_mode,'NOARCHIVELOG',1,'ARCHIVELOG',2,'MANUAL',3,0) log_mode, \
	TO_CHAR(cast(d.resetlogs_time as timestamp WITH time zone),'YYYY-MM-DD_HH24_MI_SS_TZH_TZM') RESETLOGS_TIME, \
	TO_CHAR(cast(d.controlfile_created as timestamp WITH time zone),'YYYY-MM-DD_HH24_MI_SS_TZH_TZM') CONTROLFILE_CREATED, \
	DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) OPEN_MODE, \
	DECODE(d.protection_mode, 'MAXIMUM PROTECTION', 1, 'MAXIMUM AVAILABILITY', 2, 'RESYNCHRONIZATION', 3, 'MAXIMUM PERFORMANCE', 4, 'UNPROTECTED', 5, 0) PROTECTION_MODE, \
	SWITCHOVER# AS SWITCHOVER_NUMBER, \
	DECODE(d.dataguard_broker, 'ENABLED', 1, 'DISABLED', 2, 0) DATAGUARD_BROKER, \
	DECODE(d.guard_status, 'ALL', 1, 'STANDBY', 2, 'NONE', 3, 0) GUARD_STATUS, \
	DECODE(d.force_logging, 'YES', 1, 'NO', 2, 0) FORCE_LOGGING, \
	DECODE(d.flashback_on, 'YES', 1, 'NO', 2, 'RESTORE POINT ONLY', 3, 0) FLASHBACK_ON, \
	DECODE(d.database_role, 'SNAPSHOT STANDBY', 1, 'LOGICAL STANDBY', 2, 'PHYSICAL STANDBY', 3, 'PRIMARY', 4, 0) DATABASE_ROLE, \
	d.dbid AS DBID, \
	d.name AS DBNAME, \
	d.platform_name AS PLATFORM_NAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND i.instance_name= '%s' \
	AND d.name = '%s'"

#define ORACLE_V12_DB_INFO_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
	DECODE(d.log_mode,'NOARCHIVELOG',1,'ARCHIVELOG',2,'MANUAL',3,0) log_mode, \
	TO_CHAR(cast(d.resetlogs_time as timestamp WITH time zone),'YYYY-MM-DD_HH24_MI_SS_TZH_TZM') RESETLOGS_TIME, \
	TO_CHAR(cast(d.controlfile_created as timestamp WITH time zone),'YYYY-MM-DD_HH24_MI_SS_TZH_TZM') CONTROLFILE_CREATED, \
	DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) OPEN_MODE, \
	DECODE(d.protection_mode, 'MAXIMUM PROTECTION', 1, 'MAXIMUM AVAILABILITY', 2, 'RESYNCHRONIZATION', 3, 'MAXIMUM PERFORMANCE', 4, 'UNPROTECTED', 5, 0) PROTECTION_MODE, \
	SWITCHOVER# AS SWITCHOVER_NUMBER, \
	DECODE(d.dataguard_broker, 'ENABLED', 1, 'DISABLED', 2, 0) DATAGUARD_BROKER, \
	DECODE(d.guard_status, 'ALL', 1, 'STANDBY', 2, 'NONE', 3, 0) GUARD_STATUS, \
	DECODE(d.force_logging, 'YES', 1, 'NO', 2, 0) FORCE_LOGGING, \
	DECODE(d.flashback_on, 'YES', 1, 'NO', 2, 'RESTORE POINT ONLY', 3, 0) FLASHBACK_ON, \
	DECODE(d.database_role, 'SNAPSHOT STANDBY', 1, 'LOGICAL STANDBY', 2, 'PHYSICAL STANDBY', 3, 'PRIMARY', 4, 0) DATABASE_ROLE, \
	DECODE(d.cdb,'YES',1,'NO',2,0) AS CDB, \
	d.dbid AS DBID, \
	d.name AS DBNAME, \
	d.platform_name AS PLATFORM_NAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND i.instance_name= '%s' \
	AND d.name = '%s'"

#define ORACLE_V12_PDB_INFO_DBS "\
SELECT i.instance_name AS INSTANCE, \
	i.host_name AS HOSTNAME, \
	pd.dbid AS PDB_DBID, \
	pd.name AS PDB_NAME, \
	DECODE(pd.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'MIGRATE', 4, 0) AS PDB_OPEN_MODE, \
	NVL(total_size, 0) AS PDB_SIZE, \
	TO_CHAR(cast(pd.open_time as timestamp WITH time zone), 'YYYY-MM-DD_HH24:MI:SS_TZH:TZM') AS PDB_OPEN_DATETIME, \
	ROUND((sysdate - cast(pd.open_time as date)) * 86400, 0) AS PDB_UPTIME, \
	ROUND(((cast(pd.open_time as date) - to_date('1970-01-01', 'YYYY-MM-DD')) * 86400), 0) AS PDB_OPEN_UNIXTIME, \
	DECODE(pd.recovery_status, 'ENABLED', 1, 'DISABLED', 0, 2) AS PDB_RECOVERY_STATUS \
FROM gv$instance i, gv$pdbs pd \
WHERE i.inst_id = pd.inst_id \
	AND i.instance_name = '%s'"

#define ORACLE_V18_PDB_INFO_DBS "\
SELECT i.instance_name AS INSTANCE, \
	i.host_name AS HOSTNAME, \
	pd.dbid AS PDB_DBID, \
	pd.name AS PDB_NAME, \
	DECODE(pd.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'MIGRATE', 4, 0) AS PDB_OPEN_MODE, \
	NVL(total_size, 0) AS PDB_SIZE, \
	TO_CHAR(cast(pd.open_time as timestamp WITH time zone), 'YYYY-MM-DD_HH24:MI:SS_TZH:TZM') AS PDB_OPEN_DATETIME, \
	ROUND((sysdate - cast(pd.open_time as date)) * 86400, 0) AS PDB_UPTIME, \
	ROUND(((cast(pd.open_time as date) - to_date('1970-01-01', 'YYYY-MM-DD')) * 86400), 0) AS PDB_OPEN_UNIXTIME, \
	DECODE(pd.recovery_status, 'ENABLED', 1, 'DISABLED', 0, 2) AS PDB_RECOVERY_STATUS, \
	DECODE(proxy_pdb, 'YES', 1, 'NO', 0, 2) AS PDB_PROXY, \
	local_undo AS PDB_LOCAL_UNDO \
FROM gv$instance i, gv$pdbs pd \
WHERE i.inst_id = pd.inst_id \
	AND i.instance_name = '%s'"

#define ORACLE_CHECK_DB_OPEN_MODE_DBS "\
SELECT DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) AS OPEN_MODE \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role = '%s' \
	AND i.status in('MOUNTED', 'OPEN') \
	AND i.instance_name = '%s' \
	AND d.name = '%s'"

#define ORACLE_CHECK_STANDBY_DB_OPEN_MODE_DBS "\
SELECT DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) AS OPEN_MODE \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role <> 'PRIMARY' \
	AND i.status in('MOUNTED', 'OPEN') \
	AND i.instance_name = '%s' \
	AND d.name = '%s'"

#define ORACLE_CHECK_INST_OPEN_MODE_WITHOUT_DBROLE_DBS "\
SELECT DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) AS OPEN_MODE \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND i.status in('MOUNTED', 'OPEN') \
	AND i.instance_name = '%s'"

#define ORACLE_CHECK_INST_OPEN_MODE_DBS "\
SELECT DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) AS OPEN_MODE \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role = '%s' \
	AND i.status in('MOUNTED', 'OPEN') \
	AND i.instance_name = '%s'"

#define ORACLE_CHECK_STANDBY_INST_OPEN_MODE_DBS "\
SELECT DECODE(d.open_mode, 'MOUNTED', 1, 'READ WRITE', 2, 'READ ONLY', 3, 'READ ONLY WITH APPLY', 4, 0) AS OPEN_MODE \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role <> 'PRIMARY' \
	AND i.status in('MOUNTED', 'OPEN') \
	AND i.instance_name = '%s'"

#define ORACLE_DB_INCARNATION_INFO_DBS "\
SELECT NVL(incarnation#, 0) AS INCARNATION \
FROM v$database_incarnation \
WHERE status = 'CURRENT'"

#define ORACLE_DB_SIZE_INFO_DBS "\
SELECT NVL(SUM(bytes), 0) AS DBSIZE \
FROM v$datafile"

#define ORACLE_PDB_SIZE_INFO_DBS "\
SELECT NVL(SUM(d.bytes), 0) AS PDB_SIZE \
FROM v$datafile d, v$pdbs p \
WHERE d.con_id = p.con_id \
	AND p.name = '%s'"

#define ORACLE_INSTANCE_PARAMETER_INFO_DBS "\
SELECT p.name AS PARAMETER, p.value AS PVALUE \
FROM gv$instance i, gv$parameter p \
WHERE i.instance_number = p.inst_id \
	AND i.instance_name = '%s' \
	AND p.name in('db_files', 'processes', 'sessions')"

#define ORACLE_V11_LASTPATCH_INFO_DBS "\
SELECT TO_CHAR(ACTION_TIME, 'DD-MON-YYYY HH24:MI:SS') AS LAST_PATCHSET_ACTION_TIME, \
	ACTION AS LAST_PATCHSET_ACTION, \
	VERSION AS LAST_PATCHSET_VERSION, \
	COMMENTS AS LAST_PATCHSET_COMMENTS \
FROM sys.registry$history  \
WHERE namespace = 'SERVER'  \
	AND ACTION IN('APPLY', 'RU_APPLY', 'UPGRADE')  \
	AND action_time = (SELECT max(action_time) \
			FROM sys.registry$history \
			WHERE namespace = 'SERVER' \
				AND ACTION IN('APPLY', 'RU_APPLY', 'UPGRADE') \
)"

#define ORACLE_V12_LASTPATCH_INFO_DBS "\
SELECT TO_CHAR(ACTION_TIME, 'DD-MON-YYYY HH24:MI:SS') AS LAST_PATCHSET_ACTION_TIME, \
	ACTION AS LAST_PATCHSET_ACTION, \
	PATCH_ID AS LAST_PATCHSET_VERSION, \
	DESCRIPTION AS LAST_PATCHSET_COMMENTS \
FROM sys.dba_registry_sqlpatch \
WHERE ACTION IN('APPLY')  \
	AND action_time = (SELECT max(action_time) \
			FROM sys.dba_registry_sqlpatch \
			WHERE ACTION IN ('APPLY') AND FLAGS IN ('N') \
)"

#define ORACLE_INSTANCE_RESOURCE_INFO_DBS "\
SELECT r.resource_name AS RNAME, \
	NVL(r.current_utilization, 0) AS RVALUE \
FROM gv$instance i, gv$resource_limit r \
WHERE i.instance_number = r.inst_id \
	AND i.instance_name = '%s' \
	AND r.resource_name in('processes', 'sessions')"

#define ORACLE_INSTANCE_DB_FILES_CURRENT_DBS "\
SELECT NVL(count(*), 0) AS DB_FILES_CURRENT \
FROM gv$instance i, gv$datafile d \
WHERE i.instance_number = d.inst_id \
	AND i.instance_name = '%s'"

#define ORACLE_INSTANCE_RESUMABLE_COUNT_DBS "\
SELECT NVL(count(rr.instance_id),0) AS RESUMABLE_COUNT \
FROM(SELECT r.instance_id FROM dba_resumable r WHERE r.STATUS = 'SUSPENDED') rr \
RIGHT JOIN gv$instance i ON i.inst_id = rr.instance_id \
GROUP BY i.INSTANCE_NAME"

#define ORACLE_INSTANCE_COUNT_BAD_PROCESSES_DBS "\
SELECT NVL(count(pp.addr), 0) AS COUNT_BAD_PROCESSES FROM \
(SELECT p.addr, inst_id \
	FROM gv$process p \
	WHERE p.program <> 'PSEUDO' \
	AND p.program NOT LIKE '%%(D00%%' AND p.program NOT LIKE '%%(S0%%' \
	AND p.program NOT LIKE '%%(S1%%' AND p.program NOT LIKE '%%(P0%%' \
	AND p.program NOT LIKE '%%(P1%%' AND p.program NOT LIKE '%%(P2%%' \
	AND p.program NOT LIKE '%%(P3%%' AND p.program NOT LIKE '%%(P4%%' \
	AND p.program NOT LIKE '%%(P5%%' AND p.program NOT LIKE '%%(P6%%' \
	AND p.program NOT LIKE '%%(P7%%' AND p.program NOT LIKE '%%(P8%%' \
	AND p.program NOT LIKE '%%(P9%%' \
	AND p.program NOT LIKE '%%(J0%%' \
	MINUS SELECT paddr, inst_id FROM gv$session \
	MINUS SELECT paddr, inst_id FROM gv$bgprocess) pp \
RIGHT JOIN gv$instance i ON i.INST_ID = pp.INST_ID \
GROUP BY i.INSTANCE_NAME"

#define ORACLE_INSTANCE_FRA_INFO_DBS "\
SELECT name AS FRA_LOCATION_NAME, \
	number_of_files AS FRA_FILE_NUM, \
	space_limit AS FRA_SPACE_LIMIT, \
	space_used AS FRA_SPACE_USED, space_reclaimable AS FRA_SPACE_RECLAIMABLE, (space_limit-(space_used-space_reclaimable)) AS FRA_SPACE_FREE, \
	DECODE(space_limit, 0, 0, ROUND(((space_used - space_reclaimable) / space_limit) * 100, 2)) AS FRA_USED_PCT, \
	DECODE(space_limit, 0, 100, ROUND(100 - ((space_used - space_reclaimable) / space_limit) * 100, 2)) AS FRA_FREE_PCT \
FROM v$recovery_file_dest"

#define ORACLE_INSTANCE_REDOLOG_SWITCH_RATE_INFO_DBS "\
WITH a AS(SELECT cnt, thread# , row_number() over(partition by thread# order by cnt desc) rn \
	FROM(SELECT count(*) cnt, a.thread#, a.dest_id \
		FROM v$archived_log a, v$archive_dest_status d \
		WHERE completion_time > sysdate-1/24 \
		AND a.dest_id = d.dest_id AND d.status = 'VALID' AND type = 'LOCAL' \
		GROUP BY a.thread#, a.dest_id \
	) \
) \
SELECT NVL(sum(cnt), 0) AS REDOLOG_SWITCH_RATE \
FROM( \
	SELECT t.thread# as thread, NVL((SELECT cnt FROM a WHERE a.thread# = t.thread# AND rn = 1), 0) cnt FROM v$thread t WHERE t.status = 'OPEN' \
)"

#define ORACLE_INSTANCE_REDOLOG_SIZE_INFO_DBS "\
SELECT NVL(ROUND(SUM(blocks * block_size)), 0) AS REDOLOG_SIZE_IN_BYTE \
FROM v$archived_log a, v$archive_dest_status d \
WHERE first_time >= sysdate-1/24 \
	AND a.dest_id = d.dest_id \
	AND d.status = 'VALID' \
	AND type = 'LOCAL'"

#define ORACLE_INSTANCE_ARCHIVE_LOG_BACKUP_INFO_DBS "\
SELECT NVL((SELECT TO_CHAR(round((sysdate-(max(c.start_time)))*24*60*60, 0)) \
FROM v$backup_set c, v$backup_piece p \
WHERE c.backup_type = 'L' \
	AND c.set_stamp = p.set_stamp \
	AND c.set_count = p.set_count), \
TO_CHAR((cast(SYS_EXTRACT_UTC(SYSTIMESTAMP) as date)-to_date('01011970', 'ddmmyyyy'))*24*60*60)) AS LAST_ARCHIVE_LOG_BACKUP FROM dual"

#define ORACLE_INSTANCE_FULL_BACKUP_INFO_DBS "\
SELECT NVL((SELECT TO_CHAR(round((sysdate-(min(completion_time)))*24*60*60,0)) \
FROM( \
	SELECT CASE \
	WHEN(e.enabled = 'READ ONLY' AND e.checkpoint_time < greatest(NVL(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')), NVL(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')))) then sysdate \
	WHEN(e.enabled = 'READ ONLY' AND e.checkpoint_time >= greatest(NVL(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')), NVL(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')))) then greatest(nvl(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')), nvl(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD'))) \
	ELSE NVL(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')) \
	END completion_time, \
	a.FILE# \
	FROM( \
		SELECT max(c.completion_time) as completion_time, c.FILE# as FILE# \
		FROM v$backup_datafile c, v$backup_piece p \
		WHERE c.FILE# > 0 AND(c.incremental_level = 0 or c.incremental_level is null) \
		AND  c.set_stamp = p.set_stamp  AND c.set_count = p.set_count \
		GROUP BY c.FILE# \
	) a, \
	(SELECT max(c.completion_time) as completion_time, c.FILE# as FILE# \
		FROM v$backup_datafile c, v$backup_piece p \
		WHERE c.file#  > 0 AND c.incremental_level > 0 \
		AND  c.set_stamp = p.set_stamp  AND c.set_count = p.set_count \
		GROUP BY c.FILE#) b, \
	v$datafile e \
	WHERE a.FILE#(+) = e.file# \
	AND  b.FILE#(+) = e.file# \
	AND e.creation_time < ( \
		SELECT max(c.completion_time) as completion_time \
		FROM v$backup_datafile c, v$backup_piece p \
		WHERE(c.incremental_level = 0 or c.incremental_level is null) AND c.file#  > 0 AND  c.set_stamp = p.set_stamp  AND c.set_count = p.set_count \
		))), \
TO_CHAR((cast(SYS_EXTRACT_UTC(SYSTIMESTAMP) as date)-to_date('01011970', 'ddmmyyyy'))*24*60*60)) AS LAST_FULL_BACKUP FROM dual"

#define ORACLE_INSTANCE_INCR_BACKUP_FILE_NUM_DBS "\
SELECT TO_CHAR(count(*)) AS LAST_INCR_BACKUP_FILE_NUM \
FROM v$backup_datafile c, v$backup_piece p \
WHERE incremental_level > 0 \
AND  c.set_stamp = p.set_stamp \
AND c.set_count = p.set_count \
AND c.completion_time > (SELECT min(NVL(completion_time, to_date('1970-01-01', 'YYYY-MM-DD'))) \
	FROM(SELECT max(c.completion_time) as completion_time, c.FILE# as FILE# \
		FROM  v$backup_datafile c, v$backup_piece p \
		WHERE c.FILE# > 0 AND(c.incremental_level = 0 or c.incremental_level is null) \
		AND  c.set_stamp = p.set_stamp  AND c.set_count = p.set_count \
		GROUP BY c.FILE# \
	) a, v$datafile e \
	WHERE a.FILE#(+) = e.file# \
	AND e.creation_time < (SELECT max(completion_time) \
		FROM(SELECT max(c.completion_time) as completion_time, c.FILE# as FILE# \
			FROM  v$backup_datafile c, v$backup_piece p \
			WHERE c.FILE# > 0 AND(c.incremental_level = 0 or c.incremental_level is null) \
			AND c.set_stamp = p.set_stamp  \
			AND c.set_count = p.set_count \
			GROUP BY c.FILE# \
		) \
		) \
)"

#define ORACLE_INSTANCE_INCR_BACKUP_INFO_DBS "\
SELECT NVL((SELECT TO_CHAR(round((sysdate-(min(completion_time)))*24*60*60, 0)) \
FROM( \
	SELECT CASE \
	WHEN(e.enabled = 'READ ONLY' AND e.checkpoint_time < greatest(NVL(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')), NVL(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')))) then sysdate \
	WHEN(e.enabled = 'READ ONLY' AND e.checkpoint_time >= greatest(NVL(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')), NVL(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')))) then greatest(nvl(a.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')), nvl(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD'))) \
	ELSE NVL(b.completion_time, to_date('1970-01-01', 'YYYY-MM-DD')) \
	END completion_time, \
	a.FILE# \
	FROM( \
		SELECT max(c.completion_time) as completion_time, c.FILE# as FILE# \
		FROM v$backup_datafile c, v$backup_piece p \
		WHERE c.FILE# > 0 AND(c.incremental_level = 0 OR c.incremental_level is null) \
		AND  c.set_stamp = p.set_stamp  \
		AND c.set_count = p.set_count \
		GROUP BY c.FILE# \
	) a, \
	(SELECT max(c.completion_time) as completion_time, c.FILE# as FILE# \
		FROM v$backup_datafile c, v$backup_piece p \
		WHERE c.file#  > 0 AND c.incremental_level > 0 \
		AND  c.set_stamp = p.set_stamp  \
		AND c.set_count = p.set_count \
		GROUP BY c.FILE#) b, \
	v$datafile e \
	WHERE a.FILE#(+) = e.file# \
	AND  b.FILE#(+) = e.file# \
	AND e.creation_time < ( \
		SELECT max(c.completion_time) as completion_time \
		FROM v$backup_datafile c, v$backup_piece p \
		WHERE(c.incremental_level = 0 or c.incremental_level is null) AND c.file#  > 0 AND c.set_stamp = p.set_stamp AND c.set_count = p.set_count \
		))), \
TO_CHAR((cast(SYS_EXTRACT_UTC(SYSTIMESTAMP) as date)-to_date('01011970', 'ddmmyyyy'))*24*60*60)) AS LAST_INCR_BACKUP FROM dual"

#define ORACLE_INSTANCE_CF_BACKUP_INFO_DBS "\
SELECT NVL((SELECT TO_CHAR(round((sysdate-(max(c.start_time)))*24*60*60, 0)) \
	FROM v$backup_set c, v$backup_piece p \
	WHERE c.controlfile_included in('YES', 'SBY') \
	AND c.set_stamp = p.set_stamp \
	AND c.set_count = p.set_count), \
TO_CHAR((cast(SYS_EXTRACT_UTC(SYSTIMESTAMP) as date)-to_date('01011970', 'ddmmyyyy'))*24*60*60)) AS LAST_CF_BACKUP FROM dual"

#define ORACLE_V11_DISCOVER_STANDBY_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
    d.name AS DBNAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role <> 'PRIMARY' \
	AND i.instance_name= '%s'"

#define ORACLE_V12_DISCOVERY_STANDBY_DBS "\
SELECT i.instance_name AS INSTANCE, \
    i.host_name AS HOSTNAME, \
    d.name AS DBNAME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role <> 'PRIMARY' \
	AND i.instance_name= '%s'"

#define ORACLE_STANDBY_LAG_DBS "\
SELECT DECODE(name, 'apply lag', 'APPLY_LAG', 'transport lag', 'TRANSPORT_LAG', 'NONE') AS PARAM_NAME, \
NVL(TO_CHAR(extract(day from to_dsinterval(value))*24*60*60+extract(hour from to_dsinterval(value))*60*60+extract(minute from to_dsinterval(value))*60+extract(second from to_dsinterval(value))), to_char(86400)) AS LAG_VALUE \
FROM v$dataguard_stats \
WHERE name IN('apply lag', 'transport lag') \
union all \
SELECT DECODE(name, 'apply lag', 'APPLY_TIME_COMPUTED', 'transport lag', 'TRANSPORT_TIME_COMPUTED', 'NONE') AS PARAM_NAME, \
	TO_CHAR(NVL(round(abs((sysdate - to_date(TIME_COMPUTED, 'mm/dd/yyyy hh24:mi:ss'))*24*60*60),0), 86400)) AS TIME_COMPUTED_LAG_VALUE \
FROM v$dataguard_stats \
WHERE name IN('apply lag', 'transport lag')"

#define ORACLE_STANDBY_MRP_STATUS_DBS "\
SELECT b.status AS MRP_STATUS, b.mrp_cnt AS MRP_CNT \
FROM(SELECT a.status, a.mrp_cnt, max(a.status) over() mx \
	FROM(SELECT DECODE(NVL(status, 'NULL'), 'UNUSED', 1, 'ALLOCATED', 2, 'CONNECTED', 3, 'ATTACHED', 4, 'IDLE', 5, 'ERROR', 6, 'OPENING', 7, 'CLOSING', 8, 'WRITING', 9, 'RECEIVING', 10, 'ANNOUNCING', 11, 'REGISTERING', 12, 'WAIT_FOR_LOG', 13, 'WAIT_FOR_GAP', 14, 'APPLYING_LOG', 15, 0) AS status, count(*) AS mrp_cnt \
		FROM v$managed_standby \
		WHERE process LIKE 'MRP%%' \
		GROUP by process, status \
		UNION ALL \
		SELECT 0, 0 FROM dual) a) b \
	WHERE mx <= status"

#define ORACLE_STANDBY_RFS_STATUS_DBS "\
SELECT i.instance_name AS INSTANCE, count(rr.inst_id) RFS_CNT FROM \
	(SELECT ms.inst_id FROM gv$managed_standby ms WHERE ms.PROCESS = 'RFS') rr \
	RIGHT JOIN gv$instance i ON i.INST_ID = rr.inst_id \
WHERE i.instance_name='%s' \
	GROUP BY i.instance_name"

#define ORACLE_STANDBY_LAST_SEQUENCE_STAT_DBS "\
SELECT distinct ARCH.THREAD# AS THREAD, ARCH.SEQUENCE# AS LAST_SEQUENCE_RECEIVED, APPL.SEQUENCE# AS LAST_SEQUENCE_APPLIED \
FROM \
	(SELECT THREAD#, max(SEQUENCE#) as SEQUENCE# FROM V$ARCHIVED_LOG WHERE(THREAD#, FIRST_TIME) IN (SELECT THREAD#, MAX(FIRST_TIME) FROM V$ARCHIVED_LOG GROUP BY THREAD#) GROUP BY THREAD#) ARCH, \
	(SELECT THREAD#, max(SEQUENCE#) as SEQUENCE# FROM V$LOG_HISTORY WHERE(THREAD#, FIRST_TIME) IN (SELECT THREAD#, MAX(FIRST_TIME) FROM V$LOG_HISTORY GROUP BY THREAD#) GROUP BY THREAD#) APPL \
WHERE ARCH.THREAD# = APPL.THREAD# ORDER BY 1"

// This SQL request calculates the transport lag without taking into account the possible difference in time zones in which there are primary and standby database
#define ORACLE_STANDBY_THREADED_TRANSPORT_LAG_DBS "\
SELECT thread# AS THREAD_NUM, TRUNC((sysdate - to_date('1970-01-01', 'YYYY-MM-DD')) * 86400) AS CURRENT_UNIXTIME, (sysdate-MAX(next_time))*86400 AS TRANSPORT_LAG \
FROM ( \
	SELECT thread#, MAX(sequence#), MAX(next_time) AS next_time FROM v$archived_log l \
	WHERE RESETLOGS_CHANGE# = (SELECT RESETLOGS_CHANGE# FROM v$database) \
		AND sequence# < NVL((SELECT MIN(cs) \
		FROM (SELECT thread#, cs FROM ( \
			SELECT thread#, sequence# AS cs, LEAD(sequence#, 1) OVER (PARTITION BY thread# ORDER BY sequence#) AS hs, \
				LEAD(sequence#, 1) OVER (PARTITION BY thread# ORDER BY sequence#) - sequence# AS dt \
				FROM v$archived_log \
			WHERE first_change# > (SELECT MIN(checkpoint_change#) FROM v$datafile_header) \
		) WHERE dt > 1 \
	) g WHERE g.thread# = l.thread# \
), sequence# + 1) \
GROUP BY thread# \
UNION ALL \
SELECT thread#, MAX(sequence#), MAX(last_time) AS next_time FROM v$standby_log l \
WHERE sequence# < NVL((select min(cs) \
FROM (SELECT thread#, cs FROM ( \
		SELECT thread#, sequence# AS cs, LEAD(sequence#, 1) OVER (PARTITION BY thread# ORDER BY sequence#) AS hs, \
			LEAD (sequence#, 1) OVER (PARTITION BY thread# ORDER BY sequence#) - sequence# AS dt \
			FROM v$archived_log \
		WHERE first_change# > (SELECT MIN(checkpoint_change#) FROM v$datafile_header) \
	) WHERE dt > 1 \
) g WHERE g.thread# = l.thread# \
), sequence# + 1) \
GROUP BY thread# \
) GROUP BY thread#"

// This SQL request get SCN from last applied redo
#define ORACLE_STANDBY_SCN_DBS "\
SELECT i.instance_name AS INSTANCE, d.name AS DBNAME, \
TO_NUMBER(SUBSTR(r.comments, 6, 21)) AS STANDBY_SCN, \
TRUNC((sysdate - to_date('1970-01-01', 'YYYY-MM-DD')) * 86400) AS CURRENT_UNIXTIME \
FROM gv$instance i, gv$database d, gv$recovery_progress r \
WHERE i.inst_id = d.inst_id  \
	AND d.database_role <> 'PRIMARY' \
	AND r.item = 'Last Applied Redo' \
	AND r.start_time = ( \
		SELECT max(start_time) from gv$recovery_progress where item = 'Last Applied Redo' \
)"

// This SQL request calculates the apply lag (run it on primary database, required enter last SCN (see ORACLE_STANDBY_SCN_DBS))
#define ORACLE_STANDBY_APPLY_LAG_DBS "\
SELECT i.instance_name AS INSTANCE, d.name AS DBNAME, \
	ROUND((sysdate - cast(scn_to_timestamp(%s) as date)) * 86400, 0) AS APPLY_LAG, \
	TRUNC((sysdate - to_date('1970-01-01', 'YYYY-MM-DD')) * 86400) AS CURRENT_UNIXTIME \
FROM gv$instance i, gv$database d \
WHERE i.inst_id = d.inst_id \
	AND d.database_role = 'PRIMARY'"

#define ORACLE_DISCOVERY_ARLDEST_DBS "\
SELECT i.INSTANCE_NAME AS INSTANCE, \
	db.name AS DBNAME, \
	bt.dest_name AS ARLDEST \
FROM gv$instance i \
JOIN gv$database db ON (db.inst_id = i.inst_id) \
JOIN gv$archive_dest bt ON (bt.inst_id = i.inst_id) \
WHERE bt.status != 'INACTIVE' \
	AND db.log_mode = 'ARCHIVELOG' \
	AND i.instance_name= '%s'"

#define ORACLE_ARLDEST_INFO_DBS "\
SELECT i.INSTANCE_NAME AS INSTANCE, \
	db.name AS DBNAME, \
	bt.dest_name AS ARLDEST, \
	DECODE(bt.status, 'VALID', 1, 'INACTIVE', 2, 'DEFERRED', 3, 'ERROR', 4, 'DISABLED', 5, 'BAD PARAM', 6, 'ALTERNATE', 7, 'FULL', 8, 0) AS LOG_STATUS, \
	DECODE(bt.target, 'PRIMARY', 1, 'STANDBY', 2, 'LOCAL', 3, 'REMOTE', 4, 0) AS LOG_TARGET, \
	DECODE(bt.archiver, 'ARCH', 1, 'FOREGROUND', 2, 'LGWR', 3, 'RFS', 4, 0) AS LOG_ARCHIVER, \
	NVL(bt.log_sequence, 0) AS LOG_SEQUENCE, \
	REPLACE(bt.error, '\"', '|') AS LOG_ERROR \
FROM gv$instance i \
	JOIN gv$database db ON (db.inst_id = i.inst_id) \
	JOIN gv$archive_dest bt ON (bt.inst_id = i.inst_id) \
WHERE bt.status != 'INACTIVE' \
	AND db.log_mode = 'ARCHIVELOG' \
	AND i.instance_name = '%s'"

#define ORACLE_INSTANCE_PARAMETERS_INFO_DBS "\
SELECT i.instance_name AS INSTANCE, \
	p.name AS PARAMETER, \
	TO_CHAR(p.value) AS PVALUE \
FROM gv$instance i, gv$parameter p \
WHERE i.instance_number = p.inst_id \
	AND p.type IN(3, 6) \
	AND p.isdefault = 'FALSE' \
	AND p.name NOT IN ('db_files', 'processes', 'sessions') \
	AND i.instance_name= '%s'"

#define ORACLE_V11_INSTANCE_SERVICES_DISCOVERY_DBS "\
SELECT i.instance_name AS INSTANCE, \
	s.name AS SERVICE_NAME \
FROM gv$services s JOIN gv$instance i ON (s.inst_id = i.inst_id) \
	AND i.instance_name= '%s'"

#define ORACLE_V12_INSTANCE_SERVICES_DISCOVERY_DBS "\
SELECT s.pdb AS PDB, \
	i.instance_name AS INSTANCE, \
	s.name AS SERVICE_NAME \
FROM gv$services s JOIN gv$instance i ON (s.inst_id = i.inst_id) \
	AND i.instance_name= '%s'"

#define ORACLE_V11_PERMANENT_TS_INFO_DBS "\
WITH \
i AS (SELECT i.instance_name, d.name FROM gv$database d, gv$instance i WHERE d.inst_id = i.inst_id), \
f AS (SELECT tablespace_name, SUM(bytes) AS file_free_space FROM dba_free_space GROUP BY tablespace_name), \
m AS (SELECT tablespace_name, SUM(bytes) AS file_size, SUM(CASE WHEN autoextensible = 'NO' THEN bytes ELSE GREATEST(bytes, maxbytes) END) file_max_size FROM dba_data_files GROUP BY tablespace_name), \
d AS (SELECT tablespace_name, status FROM dba_tablespaces WHERE contents = 'PERMANENT'), \
df AS (SELECT tablespace_name, count(*) AS df_cnt FROM dba_data_files GROUP BY tablespace_name) \
SELECT i.instance_name AS INSTANCE, \
	i.name AS DBNAME, \
	d.tablespace_name AS TSNAME, \
	df.df_cnt AS DF_NUMBER, \
	DECODE(d.status, 'ONLINE', 1, 'OFFLINE', 2, 'READ ONLY', 3, 0) AS TS_STATUS, \
	ROUND(NVL(f.file_free_space, 0)) AS TS_FILE_FREE_SPACE, \
	ROUND(NVL(m.file_size, 0)) AS TS_FILE_SIZE, \
	ROUND(NVL(m.file_max_size, 0)) AS TS_FILE_MAX_SIZE \
FROM i, f, m, d, df \
WHERE d.tablespace_name = f.tablespace_name(+) \
	AND d.tablespace_name = m.tablespace_name(+) \
	AND d.tablespace_name = df.tablespace_name(+) \
	AND i.instance_name= '%s'"

#define ORACLE_V12_PERMANENT_TS_INFO_DBS "\
WITH \
d AS (SELECT i.instance_name, p.con_id, DECODE(p.con_id, 0, d.name, p.name) AS dbname FROM v$containers p, v$database d, v$instance i), \
tbs AS (SELECT t.con_id, t.tablespace_name, DECODE(t.status, 'ONLINE', 1, 'OFFLINE', 2, 'READ ONLY', 3, 0) ts_status, COUNT(file_id) df_cnt FROM cdb_tablespaces t, cdb_data_files d WHERE t.con_id = d.con_id AND t.tablespace_name = d.tablespace_name AND t.contents = 'PERMANENT' GROUP BY t.con_id, t.tablespace_name, t.status), \
free AS (SELECT con_id, tablespace_name, SUM(bytes) file_free_space FROM cdb_free_space WHERE(con_id, tablespace_name) IN(SELECT con_id, tablespace_name FROM tbs) GROUP BY con_id, tablespace_name), \
maxsz AS (SELECT con_id, tablespace_name, SUM(bytes) file_size, SUM(CASE WHEN autoextensible = 'NO' THEN bytes ELSE GREATEST(bytes, maxbytes) END) file_max_size FROM cdb_data_files WHERE(con_id, tablespace_name) IN(SELECT con_id, tablespace_name FROM tbs) GROUP BY con_id, tablespace_name), \
spc AS( \
	SELECT m.con_id, m.tablespace_name, \
	ROUND(NVL(f.file_free_space, 0)) AS TS_FILE_FREE_SPACE, \
	ROUND(NVL(m.file_size, 0)) AS TS_FILE_SIZE, \
	ROUND(NVL(m.file_max_size, 0)) AS TS_FILE_MAX_SIZE \
	FROM maxsz m, free f WHERE m.con_id = f.con_id(+) AND m.tablespace_name = f.tablespace_name(+)) \
SELECT ts.con_id AS CONID, d.instance_name AS INSTANCE, d.dbname AS DBNAME, ts.tablespace_name AS TSNAME, ts.df_cnt AS DF_NUMBER, ts.ts_status AS TS_STATUS, s.TS_FILE_FREE_SPACE AS TS_FILE_FREE_SPACE, s.TS_FILE_SIZE AS TS_FILE_SIZE, s.TS_FILE_MAX_SIZE AS TS_FILE_MAX_SIZE \
	FROM spc s, tbs ts, d \
	WHERE d.con_id = s.con_id \
	AND s.con_id = ts.con_id \
	AND s.tablespace_name = ts.tablespace_name \
	AND d.instance_name= '%s'"

#define ORACLE_V11_TEMPORARY_TS_INFO_DBS "\
WITH \
i AS(SELECT i.instance_name, d.name FROM gv$database d, gv$instance i WHERE d.inst_id = i.inst_id), \
f AS (SELECT t.tablespace_name tablespace_name, NVL(NVL((SELECT sum(bytes) FROM dba_temp_files tf WHERE tf.tablespace_name = t.tablespace_name), 0) - NVL((SELECT sum(s.blocks) FROM v$tempseg_usage s WHERE t.tablespace_name = s.tablespace), 0)*t.block_size, 0) as tbs_free from dba_tablespaces t WHERE t.contents = 'TEMPORARY' GROUP BY t.tablespace_name, t.block_size), \
m AS (SELECT d.tablespace_name tablespace_name, SUM(d.bytes) AS file_size, SUM(CASE WHEN autoextensible = 'NO' THEN bytes ELSE GREATEST(bytes, maxbytes) END) file_max_size FROM dba_temp_files d GROUP BY d.tablespace_name), \
d AS (SELECT tablespace_name, status FROM dba_tablespaces WHERE contents = 'TEMPORARY'), \
df AS (SELECT tablespace_name, count(*) AS df_cnt FROM dba_temp_files GROUP BY tablespace_name) \
SELECT i.instance_name AS INSTANCE, \
	i.name AS DBNAME, \
	d.tablespace_name AS TSNAME, \
	df.df_cnt AS DF_NUMBER, \
	DECODE(d.status, 'ONLINE', 1, 'OFFLINE', 2, 'READ ONLY', 3, 0) AS TS_STATUS, \
	ROUND(NVL(f.tbs_free, 0)) AS TS_FILE_FREE_SPACE, \
	ROUND(NVL(m.file_size, 0)) AS TS_FILE_SIZE, \
	ROUND(NVL(m.file_max_size, 0)) AS TS_FILE_MAX_SIZE \
FROM i, f, m, d, df \
WHERE d.tablespace_name = f.tablespace_name(+) \
	AND d.tablespace_name = m.tablespace_name(+) \
	AND d.tablespace_name = df.tablespace_name(+) \
	AND i.instance_name= '%s'"

#define ORACLE_V12_TEMPORARY_TS_INFO_DBS "\
WITH \
d AS (SELECT i.instance_name, p.con_id, DECODE(p.con_id, 0, d.name, p.name) dbname FROM v$containers p, v$database d, v$instance i), \
m AS (SELECT con_id, tablespace_name, SUM(bytes) file_size, SUM(CASE WHEN autoextensible = 'NO' THEN bytes ELSE GREATEST(bytes, maxbytes) END) file_max_size, count(*) df_cnt FROM cdb_temp_files GROUP BY con_id, tablespace_name), \
u AS (SELECT con_id,tablespace tablespace_name,SUM(blocks) blocks FROM v$tempseg_usage group by con_id,tablespace), \
t AS (SELECT con_id, tablespace_name, block_size, status FROM cdb_tablespaces WHERE contents='TEMPORARY'), \
a AS (SELECT m.con_id, m.tablespace_name, m.file_size, m.file_max_size, m.df_cnt, NVL(u.blocks,0) blocks_used FROM m,u WHERE m.con_id=u.con_id(+) AND m.tablespace_name=u.tablespace_name(+)) \
SELECT d.con_id AS CONID, \
	d.instance_name AS INSTANCE, \
	d.dbname AS DBNAME, \
	a.tablespace_name AS TSNAME, \
	a.df_cnt AS DF_NUMBER, \
	DECODE(t.status, 'ONLINE', 1, 'OFFLINE', 2, 'READ ONLY', 3, 0) AS TS_STATUS, \
	ROUND(NVL(a.file_size-(a.blocks_used*t.block_size),0)) AS TS_FILE_FREE_SPACE, \
	ROUND(NVL(a.file_size, 0)) AS TS_FILE_SIZE, \
	ROUND(NVL(a.file_max_size, 0)) AS TS_FILE_MAX_SIZE \
FROM d,t,a WHERE d.con_id=t.con_id \
	AND t.con_id=a.con_id \
	AND t.tablespace_name=a.tablespace_name \
	AND d.instance_name= '%s'"

#define ORACLE_V11_UNDO_TS_INFO_DBS "\
WITH \
i AS (SELECT i.instance_name, d.name FROM gv$database d, gv$instance i WHERE d.inst_id = i.inst_id), \
f AS (SELECT ue.tablespace_name, SUM(ue.bytes * decode(ue.status, 'ACTIVE', 1, decode(ts.RETENTION, 'GUARANTEE', 1, 0))) AS usedbytes FROM dba_undo_extents ue, dba_tablespaces ts WHERE ue.status in ('ACTIVE', 'UNEXPIRED') AND ts.TABLESPACE_NAME = ue.TABLESPACE_NAME GROUP BY ue.tablespace_name), \
m AS (SELECT tablespace_name, SUM(bytes) AS file_size, SUM(CASE WHEN autoextensible = 'NO' THEN bytes ELSE GREATEST(bytes, maxbytes) END) AS file_max_size FROM dba_data_files GROUP BY tablespace_name), \
d AS (SELECT tablespace_name, status FROM dba_tablespaces WHERE contents = 'UNDO'), \
df AS (SELECT tablespace_name, count(*) AS df_cnt FROM dba_data_files GROUP BY tablespace_name) \
SELECT i.instance_name AS INSTANCE, \
	i.name AS DBNAME, \
	d.tablespace_name AS TSNAME, \
	df.df_cnt AS DF_NUMBER, \
	DECODE(d.status, 'ONLINE', 1, 'OFFLINE', 2, 'READ ONLY', 3, 0) AS TS_STATUS, \
	ROUND(NVL(f.usedbytes, 0)) AS TS_USED_BYTES, \
	ROUND(NVL(m.file_size, 0)) AS TS_FILE_SIZE, \
	ROUND(NVL(m.file_max_size, 0)) AS TS_FILE_MAX_SIZE \
FROM i, f, m, d, df \
WHERE d.tablespace_name = f.tablespace_name(+) \
	AND d.tablespace_name = m.tablespace_name(+) \
	AND d.tablespace_name = df.tablespace_name(+) \
	AND i.instance_name= '%s'"

#define ORACLE_V12_UNDO_TS_INFO_DBS "\
WITH \
d AS (SELECT i.instance_name, p.con_id, DECODE(p.con_id, 0, d.name, p.name) dbname FROM v$containers p, v$database d, v$instance i), \
f AS (SELECT ue.con_id, ue.tablespace_name, SUM(ue.bytes * decode(ue.status, 'ACTIVE', 1, decode(ts.RETENTION, 'GUARANTEE', 1, 0))) AS usedbytes \
	FROM cdb_undo_extents ue, cdb_tablespaces ts WHERE ue.status in ('ACTIVE', 'UNEXPIRED') and ue.con_id = ts.con_id GROUP BY ue.con_id, ue.tablespace_name), \
tbs AS (SELECT t.con_id, t.tablespace_name, SUM(bytes) AS file_size, SUM(CASE WHEN autoextensible = 'NO' THEN bytes ELSE GREATEST(bytes, maxbytes) END) AS file_max_size, DECODE(t.status, 'ONLINE', 1, 'OFFLINE', 2, 'READ ONLY', 3, 0) ts_status, COUNT(file_id) df_cnt \
	FROM cdb_tablespaces t, cdb_data_files d WHERE t.con_id=d.con_id AND t.tablespace_name=d.tablespace_name AND t.contents='UNDO' GROUP BY t.con_id,t.tablespace_name,t.status) \
SELECT d.con_id AS CONID, \
	d.instance_name AS INSTANCE, \
	d.dbname AS DBNAME, \
	tbs.tablespace_name AS TSNAME, \
	tbs.df_cnt AS DF_NUMBER, \
	tbs.ts_status AS TS_STATUS, \
	ROUND(NVL(f.usedbytes, 0)) AS TS_USED_BYTES, \
	ROUND(NVL(tbs.file_size, 0)) AS TS_FILE_SIZE, \
	ROUND(NVL(tbs.file_max_size, 0)) AS TS_FILE_MAX_SIZE \
FROM d,f,tbs \
WHERE d.con_id=tbs.con_id \
	AND f.con_id(+)=tbs.con_id \
	AND f.tablespace_name(+)=tbs.tablespace_name \
	AND d.instance_name= '%s'"

// Oracle v8i/9i/10g/11g (get alert log path)
#define ORACLE_V11_ALERTLOG_INFO_DBS "\
SELECT regexp_replace(value,'(.*)([/\\])(trace|bdump|BDUMP)$','\\1\\2\\3\\2')||( \
	SELECT 'alert_' || instance_name || '.log' FROM v$instance) AS ALERTLOG \
FROM v$parameter \
WHERE name = 'background_dump_dest'"

// Oracle v11g/12c/18c (get alert log path)
#define ORACLE_V12_ALERTLOG_INFO_DBS "\
SELECT regexp_replace(value,'(.*)([/\\])(trace)$','\\1\\2\\3\\2')||( \
	SELECT 'alert_'||instance_name||'.log' FROM v$instance) AS ALERTLOG \
FROM v$diag_info WHERE name='Diag Trace'"

// Oracle v8i/9i/10g/11g (get discovery alert log path)
#define ORACLE_V11_ALERTLOG_DISCOVERY_DBS "\
SELECT i.instance_name AS INSTANCE, regexp_replace(value, '(.*)([/\\])(trace|bdump|BDUMP)$', '\\1\\2\\3\\2') || ( \
	SELECT 'alert_' || instance_name || '.log' FROM v$instance) AS ALERTLOG \
FROM gv$instance i, gv$parameter p \
WHERE i.instance_number = p.inst_id \
	AND p.name = 'background_dump_dest' \
	AND i.instance_name= '%s'"

// Oracle v11g/12c/18c (get discovery alert log path)
#define ORACLE_V12_ALERTLOG_DISCOVERY_DBS "\
SELECT i.instance_name AS INSTANCE, regexp_replace(value, '(.*)([/\\])(trace|bdump|BDUMP)$', '\\1\\2\\3\\2') || ( \
	SELECT 'alert_' || instance_name || '.log' FROM v$instance) AS ALERTLOG \
FROM gv$instance i, gv$diag_info d \
WHERE i.inst_id = d.inst_id \
	AND d.name = 'Diag Trace' \
	AND i.instance_name= '%s'"

#define ORACLE_AUDIT_FILE_DEST_DISCOVERY_DBS "\
SELECT i.INSTANCE_NAME AS INSTANCE, p.value AS AUDITFILEDEST \
FROM gv$instance i, gv$parameter p \
WHERE i.instance_number = p.inst_id \
	AND p.name = 'audit_file_dest' \
	AND i.instance_name= '%s'"

#define ORACLE_ASM_INSTANCE_INFO_DBS "\
SELECT host_name AS HOSTNAME, \
	version AS VERSION, \
	TO_CHAR(round(((sysdate - startup_time) * 60 * 60 * 24), 0)) AS UPTIME, \
	DECODE(status, 'STARTED', 1, 'MOUNTED', 2, 'OPEN', 3, 'OPEN MIGRATE', 4, 0) AS STATUS, \
	DECODE(parallel, 'YES', 1, 'NO', 2, 0) PARALLEL, \
	DECODE(archiver, 'STOPPED', 1, 'STARTED', 2, 'FAILED', 3, 0) ARCHIVER \
FROM v$instance \
WHERE instance_name = '%s'"

#define ORACLE_ASM_DISK_GROUP_INFO_DBS "\
SELECT name AS DG_NAME, \
	nvl(total_mb*1024*1024,0) AS DG_TOTAL, \
	(nvl(total_mb,0)-nvl(free_mb,0))*1024*1024 AS DG_USED, \
	CASE WHEN total_mb = 0 THEN 100 ELSE round((free_mb/total_mb)*100,2) END AS DG_FREE_PCT, \
	offline_disks AS OFFLINE_DISK, \
	decode(type,'EXTERN',1,'NORMAL',2,'HIGH',3,0) AS DG_TYPE, \
	decode(state,'CONNECTED',1,'BROKEN',2,'UNKNOWN',3,'DISMOUNTED',4,'MOUNTED',5,'QUIESCING',6,0) AS DG_STATE \
FROM v$asm_diskgroup"

#define ORACLE_ASM_TOTAL_DISK_IN_DISKGROUP_DBS "\
SELECT dg.name AS DG_NAME, NVL(count(*),0) AS TOTAL_DISK \
FROM v$asm_diskgroup dg, v$asm_disk d \
WHERE dg.group_number=d.group_number GROUP BY dg.name"

#define ORACLE_ASM_TOTAL_DISKGROUP_DBS "\
SELECT NVL(count(*),0) AS TOTAL_DISKGROUP \
FROM v$asm_diskgroup"

#define ORACLE_ASM_TOTAL_DISK_DBS "\
SELECT NVL(count(*),0) AS TOTAL_DISKS \
FROM v$asm_diskgroup dg, v$asm_disk d \
WHERE dg.group_number=d.group_number"

#define ORACLE_ASM_DISK_INFO_DBS "\
SELECT dg.name AS DG_NAME, \
	decode(d.name,'',dg.name,d.name) AS DISK_NAME, \
	d.path AS DISK_FILE_PATH, \
	d.failgroup AS DISK_FAIL_GROUP, \
	nvl(d.total_mb,0)*1024*1024 AS DISK_TOTAL, \
	(nvl(d.total_mb,0)-nvl(d.free_mb,0))*1024*1024 AS DISK_USED, \
	CASE WHEN d.total_mb = 0 THEN 100 ELSE round((d.free_mb/d.total_mb)*100,2) END AS DISK_FREE_PCT, \
	decode(d.state,'UNKNOWN',1,'NORMAL',2,'ADDING',3,'DROPPING',4,'HUNG',5,'FORCING',6,'DROPPED',7,0) AS DISK_STATE, \
	decode(d.mode_status,'ONLINE',1,'OFFLINE',2,0) DISK_MODE_STATUS  \
FROM v$asm_diskgroup dg, v$asm_disk d \
WHERE dg.group_number=d.group_number \
ORDER BY dg.name"

#define ORACLE_BLOCKED_INFO_DBS "\
SELECT NVL(count(*), 0) AS BLOCKED_NUM \
FROM gv$session a, gv$locked_object b, dba_objects c \
WHERE b.object_id = c.object_id AND a.sid = b.session_id AND a.event \
LIKE 'enq: T%%lock contention'"

#define ORACLE_ACTIVE_SESSION_CNT_DBS "\
SELECT NVL(count(*), 0) AS SESSION_CNT \
FROM gv$instance i, gv$session s \
WHERE i.instance_number = s.inst_id \
  AND i.instance_name = '%s' \
  AND s.username is not null \
  AND s.status = 'ACTIVE'"

#define ORACLE_PROCESSES_CNT_DBS "\
SELECT NVL(count(*), 0) AS PROCESS_CNT \
FROM gv$instance i, gv$process p \
WHERE i.instance_number = p.inst_id \
  AND i.instance_name = '%s' \
  AND p.username is not null"

#define ORACLE_ACTIVE_SESSION_BY_USER_MACHINE_CNT_DBS "\
SELECT DISTINCT s.username AS USERNAME, s.machine AS MACHINE, count(*) AS PROCESS_CNT \
FROM gv$session s \
LEFT JOIN gv$instance i ON s.inst_id = i.instance_number \
LEFT JOIN gv$process p ON s.paddr = p.addr AND s.inst_id =  p.inst_id \
WHERE s.username is not null \
  AND s.status = 'ACTIVE' \
  AND i.instance_name = '%s' \
GROUP BY s.username, s.machine \
ORDER BY 3 DESC"

ZBX_METRIC	parameters_dbmon_oracle[] =
/*	KEY											FLAG				FUNCTION						TEST PARAMETERS */
{
	{"oracle.instance.ping",					CF_HAVEPARAMS,		ORACLE_INSTANCE_PING,			NULL},
	{"oracle.instance.version",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.info",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.parameter",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.patch_info",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.resource",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.dbfiles",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.resumable",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.bad_processes",			CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.fra",						CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.redolog_switch_rate",		CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.redolog_size_per_hour",	CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.blocked",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.active_session_cnt",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.processes_cnt",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.active_session_by_user_machine_cnt",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.backup.archivelog",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.backup.full",						CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.backup.incr",						CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.backup.incr_file_num",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.backup.cf",						CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.db.discovery",						CF_HAVEPARAMS,		ORACLE_DISCOVERY,				NULL},
	{"oracle.db.info",							CF_HAVEPARAMS,		ORACLE_DB_INFO,					NULL},
	{"oracle.db.incarnation",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.db.size",							CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.pdb.discovery",					CF_HAVEPARAMS,		ORACLE_DISCOVERY,				NULL},
	{"oracle.pdb.info",							CF_HAVEPARAMS,		ORACLE_PDB_INFO,				NULL},
	{"oracle.standby.discovery",				CF_HAVEPARAMS,		ORACLE_DISCOVERY,				NULL},
	{"oracle.standby.lag",						CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.standby.mrp_status",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.standby.rfs_status",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.standby.last_sequence_stat",		CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.standby.transport_lag",			CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.standby.last_scn",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.standby.apply_lag",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.archlogdest.discovery",			CF_HAVEPARAMS,		ORACLE_DISCOVERY,				NULL},
	{"oracle.archlogdest.info",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.instance.parameters",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.tablespace.info",					CF_HAVEPARAMS,		ORACLE_TS_INFO,					NULL},
	{"oracle.alertlog.discovery",				CF_HAVEPARAMS,		ORACLE_DISCOVERY,				NULL},
	{"oracle.auditfiledest.discovery",			CF_HAVEPARAMS,		ORACLE_DISCOVERY,				NULL},
	{"oracle.asm.instance.ping",				CF_HAVEPARAMS,		ORACLE_INSTANCE_PING,			NULL},
	{"oracle.asm.instance.info",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.asm.diskgroup.info",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.asm.diskgroup.disk_total",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.asm.diskgroup.total",				CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.asm.disk.info",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.asm.disk.total",					CF_HAVEPARAMS,		ORACLE_GET_INSTANCE_RESULT,		NULL},
	{"oracle.query.nojson",						CF_HAVEPARAMS,		ORACLE_QUERY,		NULL},
	{"oracle.query.onerow",						CF_HAVEPARAMS,		ORACLE_QUERY,		NULL},
	{"oracle.query.twocoll",					CF_HAVEPARAMS,		ORACLE_QUERY,		NULL},
	{"oracle.query.multirow",					CF_HAVEPARAMS,		ORACLE_QUERY,		NULL},
	{"oracle.query.discovery",					CF_HAVEPARAMS,		ORACLE_QUERY,		NULL},
	{NULL}
};

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	oracle_instance_ping(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	oracle_instance_ping(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	char						*check_error = NULL, *oracle_conn_string, *oracle_str_mode, *oracle_instance, *conn_error = NULL;
	unsigned short				oracle_mode = ZBX_DB_OCI_DEFAULT;
	struct zbx_db_connection	*oracle_conn;

	if (NULL == CONFIG_ORACLE_USER)
		CONFIG_ORACLE_USER = zbx_strdup(CONFIG_ORACLE_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PASSWORD)
		CONFIG_ORACLE_PASSWORD = zbx_strdup(CONFIG_ORACLE_PASSWORD, ORACLE_DEFAULT_PASSWORD);
	if (NULL == CONFIG_ORACLE_INSTANCE)
		CONFIG_ORACLE_INSTANCE = zbx_strdup(CONFIG_ORACLE_INSTANCE, ORACLE_DEFAULT_INSTANCE);

	if (3 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (3 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few options."));
		return SYSINFO_RET_FAIL;
	}

	oracle_conn_string = get_rparam(request, 0);
	oracle_instance = get_rparam(request, 1);
	oracle_str_mode = get_rparam(request, 2);

	if (NULL == oracle_conn_string || '\0' == *oracle_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (connection string)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_instance || '\0' == *oracle_instance)
	{
		oracle_instance = CONFIG_ORACLE_INSTANCE;
	}

	if (FAIL == zbx_check_oracle_instance_name(oracle_instance, &check_error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "Invalid \"Instance name\" parameter: '%s': %s", oracle_instance, check_error);
		zbx_free(check_error);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter (instance name)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_str_mode || '\0' == *oracle_str_mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_mode, &oracle_mode))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits."));
			return SYSINFO_RET_FAIL;
		}
		
		if (5 < oracle_mode)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits (0 - 5)."));
			return SYSINFO_RET_FAIL;
		}
	}

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the oracle_instance_ping() prototype as required by */
	/* zbx_execute_dbmon_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	if (1 == CONFIG_ORACLE_USE_LOCAL_ENV)
		oracle_conn_string = NULL;

	oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_USER, CONFIG_ORACLE_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);

	if (oracle_conn != NULL)
	{
		SET_UI64_RESULT(result, 1);
		zbx_db_close_db(oracle_conn);
		zbx_db_clean_connection(oracle_conn);
	}
	else
	{
		SET_UI64_RESULT(result, 0);
	}

	zbx_free(conn_error);

	return SYSINFO_RET_OK;
}

int	ORACLE_INSTANCE_PING(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	return zbx_execute_dbmon_threaded_metric(oracle_instance_ping, request, result);
}

static int	oracle_make_result(AGENT_REQUEST *request, AGENT_RESULT *result, const char *query, zbx_db_result_type result_type, zbx_db_oracle_db_role oracle_need_db_role, unsigned int oracle_need_open_mode, zbx_db_oracle_db_status oracle_need_dbstatus)
{
	int							ret = SYSINFO_RET_FAIL, ping = 0;
	char						*check_error = NULL, *oracle_conn_string, *oracle_str_mode, *oracle_instance, *oracle_dbname, *oracle_standby_scn, *conn_error = NULL, *ora_version;
	unsigned short				oracle_mode = ZBX_DB_OCI_DEFAULT;
	unsigned int				oracle_db_open_mode = 0, oracle_db_status = 0;
	struct zbx_db_connection	*oracle_conn;
	struct zbx_db_result		ora_result;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (NULL == CONFIG_ORACLE_USER)
		CONFIG_ORACLE_USER = zbx_strdup(CONFIG_ORACLE_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PASSWORD)
		CONFIG_ORACLE_PASSWORD = zbx_strdup(CONFIG_ORACLE_PASSWORD, ORACLE_DEFAULT_PASSWORD);
	if (NULL == CONFIG_ORACLE_INSTANCE)
		CONFIG_ORACLE_INSTANCE = zbx_strdup(CONFIG_ORACLE_INSTANCE, ORACLE_DEFAULT_INSTANCE);
	if (NULL == CONFIG_ORACLE_PRIMARY_USER)
		CONFIG_ORACLE_PRIMARY_USER = zbx_strdup(CONFIG_ORACLE_PRIMARY_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PRIMARY_PASSWORD)
		CONFIG_ORACLE_PRIMARY_PASSWORD = zbx_strdup(CONFIG_ORACLE_PRIMARY_PASSWORD, ORACLE_DEFAULT_PASSWORD);

	oracle_conn_string = get_rparam(request, 0);
	oracle_instance = get_rparam(request, 1);
	oracle_str_mode = get_rparam(request, 2);
	oracle_dbname = get_rparam(request, 3);

	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): request->nparam = %d", __func__, request->key, request->nparam);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[0] = %s", __func__, request->key, oracle_conn_string);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[1] = %s", __func__, request->key, oracle_instance);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[2] = %s", __func__, request->key, oracle_str_mode);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[3] = %s", __func__, request->key, oracle_dbname);

	if (0 == strcmp(request->key, "oracle.standby.apply_lag"))
	{
		oracle_standby_scn = get_rparam(request, 4);
		zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[4] = %s", __func__, request->key, oracle_standby_scn);

		if (NULL == oracle_standby_scn || '\0' == *oracle_standby_scn)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter (standby scn)."));
			return SYSINFO_RET_FAIL;
		}
	}

	if (NULL == oracle_conn_string || '\0' == *oracle_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (connection string)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_instance || '\0' == *oracle_instance)
	{
		oracle_instance = CONFIG_ORACLE_INSTANCE;
	}

	if (FAIL == zbx_check_oracle_instance_name(oracle_instance, &check_error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "Invalid \"Instance name\" parameter: '%s': %s", oracle_instance, check_error);
		zbx_free(check_error);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (instance name)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_str_mode || '\0' == *oracle_str_mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_mode, &oracle_mode))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits."));
			return SYSINFO_RET_FAIL;
		}

		if (5 < oracle_mode)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits (0 - 5)."));
			return SYSINFO_RET_FAIL;
		}
	}

	if (NULL != oracle_dbname && '\0' != *oracle_dbname)
	{
		if (0 != isdigit(*oracle_dbname))
			oracle_dbname = NULL;
		else
		{
			if (FAIL == zbx_check_oracle_instance_name(oracle_dbname, &check_error))
			{
				zabbix_log(LOG_LEVEL_CRIT, "Invalid \"Database name\" parameter: '%s': %s", oracle_dbname, check_error);
				zbx_free(check_error);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fifth parameter (database name)."));
				return SYSINFO_RET_FAIL;
			}
		}
	}

	if (0 == strcmp(request->key, "oracle.standby.apply_lag"))
	{
		oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_PRIMARY_USER, CONFIG_ORACLE_PRIMARY_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);
	}
	else
	{
		if (1 == CONFIG_ORACLE_USE_LOCAL_ENV)
			oracle_conn_string = NULL;

		oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_USER, CONFIG_ORACLE_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);
	}

	if (NULL != oracle_conn)
	{
		if (ORA_ANY_STATUS != oracle_need_dbstatus)
		{
			if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_CHECK_SUSPEND_MODE_DBS, oracle_instance))
			{
				oracle_db_status = get_int_one_result(request, result, 0, 0, ora_result);
				zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Need_DBStatus: %s, Current_DBStatus: %s", __func__, request->key, oracle_instance, ORA_DB_STATUS[oracle_need_dbstatus], ORA_DB_STATUS[oracle_db_status]);
				zbx_db_clean_result(&ora_result);

				if (ORA_ACTIVE == oracle_db_status)
					goto next;
				else if (ORA_ANY_STATUS == oracle_db_status)
				{
					zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Oracle instance '%s' not found.", __func__, request->key, oracle_instance);
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Oracle instance not found"));
					ret = SYSINFO_RET_FAIL;
					zbx_db_close_db(oracle_conn);
					zbx_db_clean_connection(oracle_conn);
					goto out;
				}
				else
				{
					zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode.", __func__, request->key);
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode."));
					ret = SYSINFO_RET_FAIL;
					zbx_db_close_db(oracle_conn);
					zbx_db_clean_connection(oracle_conn);
					goto out;
				}
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
next:
		if (NULL == oracle_dbname || '\0' == *oracle_dbname)
		{
			if (ORA_ANY_ROLE != oracle_need_db_role)
			{
				if (ORA_STANDBY == oracle_need_db_role)
				{
					if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_CHECK_STANDBY_INST_OPEN_MODE_DBS, oracle_instance))
					{
						oracle_db_open_mode = get_int_one_result(request, result, 0, 0, ora_result);
						zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Need_Role: %s, Need_Open_Mode: %u, Current_Open_Mode: %u", __func__, request->key, oracle_instance, ORA_DB_ROLE[oracle_need_db_role], oracle_need_open_mode, oracle_db_open_mode);
						zbx_db_clean_result(&ora_result);

						if (oracle_db_open_mode > oracle_need_open_mode)
						{
							goto exec_inst_query;
						}
						else
						{
							SET_MSG_RESULT(result, zbx_strdup(NULL, "Database closed for reading information (may be not standby or open mode not mounted or read-only or read-only-with-apply)"));
							ret = SYSINFO_RET_FAIL;
						}
					}
					else
					{
						zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
						ret = SYSINFO_RET_FAIL;
					}
				}
				else
				{
					if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_CHECK_INST_OPEN_MODE_DBS, ORA_DB_ROLE[oracle_need_db_role], oracle_instance))
					{
						oracle_db_open_mode = get_int_one_result(request, result, 0, 0, ora_result);
						zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Need_Role: %s, Need_Open_Mode: %u, Current_Open_Mode: %u", __func__, request->key, oracle_instance, ORA_DB_ROLE[oracle_need_db_role], oracle_need_open_mode, oracle_db_open_mode);
						zbx_db_clean_result(&ora_result);

						if (oracle_db_open_mode > oracle_need_open_mode)
						{
							goto exec_inst_query;
						}
						else
						{
							SET_MSG_RESULT(result, zbx_strdup(NULL, "Instance closed for reading information (may be database not primary or not open with read-only or read-write)"));
							ret = SYSINFO_RET_FAIL;
						}
					}
					else
					{
						zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
						ret = SYSINFO_RET_FAIL;
					}
				}
			}
			else
			{
exec_inst_query:
				if (0 == strcmp(request->key, "oracle.standby.apply_lag"))
				{
					ret = zbx_db_query_select(oracle_conn, &ora_result, query, oracle_standby_scn);
				}
				else if (0 == strcmp(request->key, "oracle.instance.patch_info"))
				{
					if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_VERSION_DBS, oracle_instance))
					{
						ora_version = get_str_one_result(request, result, 0, 0, ora_result);

						if (NULL != ora_version)
						{
							zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version: %s", __func__, request->key, ora_version);

							if (0 > zbx_strcmp_natural(ora_version, "12.0.0.0.0"))
							{
								query = ORACLE_V11_LASTPATCH_INFO_DBS;
								zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version < 12, use query '%s'", __func__, request->key, query);
							}
							else
							{
								query = ORACLE_V12_LASTPATCH_INFO_DBS;
								zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version >= 12, use query '%s'", __func__, request->key, query);
							}
						}

						zbx_db_clean_result(&ora_result);

						ret = zbx_db_query_select(oracle_conn, &ora_result, query, oracle_instance);
					}
					else
					{
						zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query "));
						ret = SYSINFO_RET_FAIL;
					}
				}
				else
				{
					ret = zbx_db_query_select(oracle_conn, &ora_result, query, oracle_instance);
				}

				if (ZBX_DB_OK == ret)
				{
					ret = make_result(request, result, ora_result, result_type);
					zbx_db_clean_result(&ora_result);
				}
				else
				{
					zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
					ret = SYSINFO_RET_FAIL;
				}
			}
		}
		else
		{
			if (ORA_ANY_ROLE != oracle_need_db_role)
			{
				if (ORA_STANDBY == oracle_need_db_role)
				{
					if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_CHECK_STANDBY_DB_OPEN_MODE_DBS, oracle_instance, oracle_dbname))
					{
						oracle_db_open_mode = get_int_one_result(request, result, 0, 0, ora_result);

						zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Database: %s, Need_Role: %s, Need_Open_Mode: %u, Current_Open_Mode: %u", __func__, request->key, oracle_instance, oracle_dbname, ORA_DB_ROLE[oracle_need_db_role], oracle_need_open_mode, oracle_db_open_mode);

						zbx_db_clean_result(&ora_result);

						if (oracle_db_open_mode > oracle_need_open_mode)
						{
							goto exec_db_query;
						}
						else
						{
							SET_MSG_RESULT(result, zbx_strdup(NULL, "Database closed for reading information (may be not standby or open mode not mounted or read-only or read-only-with-apply)"));
							ret = SYSINFO_RET_FAIL;
						}
					}
					else
					{
						zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
						ret = SYSINFO_RET_FAIL;
					}
				}
				else
				{
					if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_CHECK_DB_OPEN_MODE_DBS, ORA_DB_ROLE[oracle_need_db_role], oracle_instance, oracle_dbname))
					{
						oracle_db_open_mode = get_int_one_result(request, result, 0, 0, ora_result);

						zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Instance: %s, Database: %s, Need_Role: %s, Need_Open_Mode: %u, Current_Open_Mode: %u", __func__, request->key, oracle_instance, oracle_dbname, ORA_DB_ROLE[oracle_need_db_role], oracle_need_open_mode, oracle_db_open_mode);

						zbx_db_clean_result(&ora_result);

						if (oracle_db_open_mode > oracle_need_open_mode)
						{
							goto exec_db_query;
						}
						else
						{
							SET_MSG_RESULT(result, zbx_strdup(NULL, "Database closed for reading information (may be not primary or not open with read-only or read-write)"));
							ret = SYSINFO_RET_FAIL;
						}
					}
					else
					{
						zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
						ret = SYSINFO_RET_FAIL;
					}
				}
			}
			else
			{
exec_db_query:
				if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, query, oracle_instance, oracle_dbname))
				{
					ret = make_result(request, result, ora_result, result_type);
					zbx_db_clean_result(&ora_result);
				}
				else
				{
					zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
					ret = SYSINFO_RET_FAIL;
				}
			}
		}

		zbx_db_close_db(oracle_conn);
		zbx_db_clean_connection(oracle_conn);
	}
	else
	{
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		if (NULL != conn_error)
			SET_MSG_RESULT(result, zbx_strdup(NULL, conn_error));
		else
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}

out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s): %s", __func__, request->key, zbx_sysinfo_ret_string(ret));

	zbx_free(conn_error);

	return ret;
}

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	oracle_get_instance_result(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	oracle_get_instance_result(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	int ret = SYSINFO_RET_FAIL;

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the oracle_get_instance_result() prototype as required by */
	/* zbx_execute_dbmon_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (0 == strcmp(request->key, "oracle.instance.version"))
	{
		ret = oracle_make_result(request, result, ORACLE_VERSION_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.instance.info"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_INFO_DBS, ZBX_DB_RES_TYPE_ONEROW, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.instance.parameter"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_PARAMETER_INFO_DBS, ZBX_DB_RES_TYPE_TWOCOLL, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.instance.patch_info"))
	{
		ret = oracle_make_result(request, result, ORACLE_V11_LASTPATCH_INFO_DBS, ZBX_DB_RES_TYPE_ONEROW, ORA_PRIMARY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.resource"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_RESOURCE_INFO_DBS, ZBX_DB_RES_TYPE_TWOCOLL, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.instance.dbfiles"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_DB_FILES_CURRENT_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.resumable"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_RESUMABLE_COUNT_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_PRIMARY, 1, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.bad_processes"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_COUNT_BAD_PROCESSES_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.fra"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_FRA_INFO_DBS, ZBX_DB_RES_TYPE_ONEROW, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.redolog_switch_rate"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_REDOLOG_SWITCH_RATE_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_PRIMARY, 1, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.redolog_size_per_hour"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_REDOLOG_SIZE_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_PRIMARY, 1, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.blocked"))
	{
		ret = oracle_make_result(request, result, ORACLE_BLOCKED_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_PRIMARY, 1, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.active_session_cnt"))
	{
		ret = oracle_make_result(request, result, ORACLE_ACTIVE_SESSION_CNT_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.processes_cnt"))
	{
		ret = oracle_make_result(request, result, ORACLE_PROCESSES_CNT_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.active_session_by_user_machine_cnt"))
	{
		ret = oracle_make_result(request, result, ORACLE_ACTIVE_SESSION_BY_USER_MACHINE_CNT_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.backup.archivelog"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_ARCHIVE_LOG_BACKUP_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.backup.full"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_FULL_BACKUP_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.backup.incr"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_INCR_BACKUP_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.backup.incr_file_num"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_INCR_BACKUP_FILE_NUM_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.backup.cf"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_CF_BACKUP_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.db.incarnation"))
	{
		ret = oracle_make_result(request, result, ORACLE_DB_INCARNATION_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.db.size"))
	{
		ret = oracle_make_result(request, result, ORACLE_DB_SIZE_INFO_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.lag"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_LAG_DBS, ZBX_DB_RES_TYPE_TWOCOLL, ORA_STANDBY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.mrp_status"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_MRP_STATUS_DBS, ZBX_DB_RES_TYPE_ONEROW, ORA_STANDBY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.rfs_status"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_RFS_STATUS_DBS, ZBX_DB_RES_TYPE_ONEROW, ORA_STANDBY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.last_sequence_stat"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_LAST_SEQUENCE_STAT_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_STANDBY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.transport_lag"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_THREADED_TRANSPORT_LAG_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_STANDBY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.apply_lag"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_APPLY_LAG_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_PRIMARY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.standby.last_scn"))
	{
		ret = oracle_make_result(request, result, ORACLE_STANDBY_SCN_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_STANDBY, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.archlogdest.info"))
	{
		ret = oracle_make_result(request, result, ORACLE_ARLDEST_INFO_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.instance.parameters"))
	{
		ret = oracle_make_result(request, result, ORACLE_INSTANCE_PARAMETERS_INFO_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ACTIVE);
	}
	else if (0 == strcmp(request->key, "oracle.asm.instance.info"))
	{
		ret = oracle_make_result(request, result, ORACLE_ASM_INSTANCE_INFO_DBS, ZBX_DB_RES_TYPE_ONEROW, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.asm.diskgroup.info"))
	{
		ret = oracle_make_result(request, result, ORACLE_ASM_DISK_GROUP_INFO_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.asm.diskgroup.disk_total"))
	{
		ret = oracle_make_result(request, result, ORACLE_ASM_TOTAL_DISK_IN_DISKGROUP_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.asm.diskgroup.total"))
	{
		ret = oracle_make_result(request, result, ORACLE_ASM_TOTAL_DISKGROUP_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.asm.disk.info"))
	{
		ret = oracle_make_result(request, result, ORACLE_ASM_DISK_INFO_DBS, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else if (0 == strcmp(request->key, "oracle.asm.disk.total"))
	{
		ret = oracle_make_result(request, result, ORACLE_ASM_TOTAL_DISK_DBS, ZBX_DB_RES_TYPE_NOJSON, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown request key"));
		ret = SYSINFO_RET_FAIL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s)", __func__, request->key);

	return ret;
}

int	ORACLE_GET_INSTANCE_RESULT(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	return zbx_execute_dbmon_threaded_metric(oracle_get_instance_result, request, result);
}

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	oracle_get_discovery(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	oracle_get_discovery(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	int							ret = SYSINFO_RET_FAIL, ping = 0;
	char						*check_error = NULL, *oracle_conn_string, *oracle_str_mode, *oracle_instance, *ora_version, *conn_error = NULL;
	unsigned short				oracle_mode = ZBX_DB_OCI_DEFAULT;
	unsigned int				oracle_db_status = 0;
	struct zbx_db_connection	*oracle_conn;
	struct zbx_db_result		ora_ver_result, ora_dbstatus_result, ora_result;
	const char					*query = ORACLE_V11_DISCOVERY_DB_DBS;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (NULL == CONFIG_ORACLE_USER)
		CONFIG_ORACLE_USER = zbx_strdup(CONFIG_ORACLE_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PASSWORD)
		CONFIG_ORACLE_PASSWORD = zbx_strdup(CONFIG_ORACLE_PASSWORD, ORACLE_DEFAULT_PASSWORD);
	if (NULL == CONFIG_ORACLE_INSTANCE)
		CONFIG_ORACLE_INSTANCE = zbx_strdup(CONFIG_ORACLE_INSTANCE, ORACLE_DEFAULT_INSTANCE);

	if (3 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (3 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few options."));
		return SYSINFO_RET_FAIL;
	}

	oracle_conn_string = get_rparam(request, 0);
	oracle_instance = get_rparam(request, 1);
	oracle_str_mode = get_rparam(request, 2);

	if (NULL == oracle_conn_string || '\0' == *oracle_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (connection string)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_instance || '\0' == *oracle_instance)
	{
		oracle_instance = CONFIG_ORACLE_INSTANCE;
	}

	if (FAIL == zbx_check_oracle_instance_name(oracle_instance, &check_error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "Invalid \"Instance name\" parameter: '%s': %s", oracle_instance, check_error);
		zbx_free(check_error);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (instance name)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_str_mode || '\0' == *oracle_str_mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_mode, &oracle_mode))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits."));
			return SYSINFO_RET_FAIL;
		}

		if (5 < oracle_mode)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits (0 - 5)."));
			return SYSINFO_RET_FAIL;
		}
	}

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the oracle_get_discovery() prototype as required by */
	/* zbx_execute_dbmon_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	if (1 == CONFIG_ORACLE_USE_LOCAL_ENV)
		oracle_conn_string = NULL;

	oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_USER, CONFIG_ORACLE_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);

	if (oracle_conn != NULL)
	{
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_dbstatus_result, ORACLE_CHECK_SUSPEND_MODE_DBS, oracle_instance))
		{
			oracle_db_status = get_int_one_result(request, result, 0, 0, ora_dbstatus_result);
			zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Need_DBStatus: %s, Current_DBStatus: %s", __func__, request->key, oracle_instance, ORA_DB_STATUS[1], ORA_DB_STATUS[oracle_db_status]);
			zbx_db_clean_result(&ora_dbstatus_result);

			if (oracle_db_status == ORA_ACTIVE)
				goto next;
			else if (oracle_db_status == ORA_ANY_STATUS)
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Oracle instance '%s' not found.", __func__, request->key, oracle_instance);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Oracle instance not found"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode.", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode."));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
			zbx_db_close_db(oracle_conn);
			zbx_db_clean_connection(oracle_conn);
			goto out;
		}
next:
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_ver_result, ORACLE_VERSION_DBS, oracle_instance))
		{
			ora_version = get_str_one_result(request, result, 0, 0, ora_ver_result);
			zbx_db_clean_result(&ora_ver_result);

			if (NULL != ora_version)
			{
				zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version: %s", __func__, request->key, ora_version);

				if (0 > zbx_strcmp_natural(ora_version, "12.0.0.0.0"))
				{
					zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version < 12, use query '%s'", __func__, request->key, query);

					if (0 == strcmp(request->key, "oracle.standby.discovery"))
					{
						query = ORACLE_V11_DISCOVER_STANDBY_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.db.discovery"))
					{
						query = ORACLE_V11_DISCOVERY_DB_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.pdb.discovery"))
					{
						query = ORACLE_V11_DISCOVERY_PDB_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.archlogdest.discovery"))
					{
						query = ORACLE_DISCOVERY_ARLDEST_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.alertlog.discovery"))
					{
						query = ORACLE_V11_ALERTLOG_DISCOVERY_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.auditfiledest.discovery"))
					{
						query = ORACLE_AUDIT_FILE_DEST_DISCOVERY_DBS;
						ret = ZBX_DB_OK;
					}
					else
					{
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown discovery request key"));
						ret = SYSINFO_RET_FAIL;
					}
				}
				else
				{
					zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version >= 12, use query '%s'", __func__, request->key, query);

					if (0 == strcmp(request->key, "oracle.standby.discovery"))
					{
						query = ORACLE_V12_DISCOVERY_STANDBY_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.db.discovery"))
					{
						query = ORACLE_V12_DISCOVERY_DB_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.pdb.discovery"))
					{
						query = ORACLE_V12_DISCOVERY_PDB_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.archlogdest.discovery"))
					{
						query = ORACLE_DISCOVERY_ARLDEST_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.alertlog.discovery"))
					{
						query = ORACLE_V12_ALERTLOG_DISCOVERY_DBS;
						ret = ZBX_DB_OK;
					}
					else if (0 == strcmp(request->key, "oracle.auditfiledest.discovery"))
					{
						query = ORACLE_AUDIT_FILE_DEST_DISCOVERY_DBS;
						ret = ZBX_DB_OK;
					}
					else
					{
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown discovery request key"));
						ret = SYSINFO_RET_FAIL;
					}
				}
			}
			else
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error get Oracle version in discovery procedure"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
			zbx_db_close_db(oracle_conn);
			zbx_db_clean_connection(oracle_conn);
			goto out;
		}

		zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle instance '%s'", __func__, request->key, oracle_instance);

		if (ZBX_DB_OK == ret)
		{
			if (zbx_db_query_select(oracle_conn, &ora_result, query, oracle_instance) == ZBX_DB_OK)
			{
				ret = make_result(request, result, ora_result, ZBX_DB_RES_TYPE_DISCOVERY);
				zbx_db_clean_result(&ora_result);
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing discovery query", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing discovery query"));
				ret = SYSINFO_RET_FAIL;
			}
		}

		zbx_db_close_db(oracle_conn);
		zbx_db_clean_connection(oracle_conn);
	}
	else
	{
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		if (NULL != conn_error)
			SET_MSG_RESULT(result, zbx_strdup(NULL, conn_error));
		else
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s): %s", __func__, request->key, zbx_sysinfo_ret_string(ret));

	zbx_free(conn_error);

	return ret;
}

int	ORACLE_DISCOVERY(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	//return zbx_execute_dbmon_threaded_metric(oracle_get_discovery, request, result);
#if !defined(_WINDOWS) && !defined(__MINGW32__)
	return oracle_get_discovery(request, result);
#else
	return oracle_get_discovery(request, result, NULL);
#endif
}

int	ORACLE_DB_INFO(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int							ret = SYSINFO_RET_FAIL;
	char						*oracle_conn_string, *oracle_str_mode, *oracle_instance, *ora_version, *oracle_dbname, *conn_error = NULL;
	unsigned short				oracle_mode = ZBX_DB_OCI_DEFAULT;
	unsigned int				oracle_db_status = 0;
	struct zbx_db_connection	*oracle_conn;
	struct zbx_db_result		ora_ver_result, ora_dbstatus_result, ora_result;
	const char					*query = ORACLE_V11_DB_INFO_DBS;

	if (NULL == CONFIG_ORACLE_USER)
		CONFIG_ORACLE_USER = zbx_strdup(CONFIG_ORACLE_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PASSWORD)
		CONFIG_ORACLE_PASSWORD = zbx_strdup(CONFIG_ORACLE_PASSWORD, ORACLE_DEFAULT_PASSWORD);
	if (NULL == CONFIG_ORACLE_INSTANCE)
		CONFIG_ORACLE_INSTANCE = zbx_strdup(CONFIG_ORACLE_INSTANCE, ORACLE_DEFAULT_INSTANCE);

	if (4 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (4 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few options."));
		return SYSINFO_RET_FAIL;
	}

	oracle_conn_string = get_rparam(request, 0);
	oracle_instance = get_rparam(request, 1);
	oracle_str_mode = get_rparam(request, 2);
	oracle_dbname = get_rparam(request, 3);

	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): request->nparam = %d", __func__, request->key, request->nparam);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[0] = %s", __func__, request->key, oracle_conn_string);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[1] = %s", __func__, request->key, oracle_instance);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[2] = %s", __func__, request->key, oracle_str_mode);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[3] = %s", __func__, request->key, oracle_dbname);

	if (NULL == oracle_conn_string || '\0' == *oracle_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (connection string)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_instance || '\0' == *oracle_instance)
	{
		oracle_instance = CONFIG_ORACLE_INSTANCE;
	}

	if (NULL == oracle_str_mode || '\0' == *oracle_str_mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_mode, &oracle_mode))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits."));
			return SYSINFO_RET_FAIL;
		}

		if (5 < oracle_mode)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits (0 - 5)."));
			return SYSINFO_RET_FAIL;
		}
	}

	if (NULL == oracle_dbname || '\0' == *oracle_dbname)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter (dbname)."));
		return SYSINFO_RET_FAIL;
	}

	if (1 == CONFIG_ORACLE_USE_LOCAL_ENV)
		oracle_conn_string = NULL;

	oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_USER, CONFIG_ORACLE_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);

	if (oracle_conn != NULL)
	{
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_dbstatus_result, ORACLE_CHECK_SUSPEND_MODE_DBS, oracle_instance))
		{
			oracle_db_status = get_int_one_result(request, result, 0, 0, ora_dbstatus_result);
			zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Need_DBStatus: %s, Current_DBStatus: %s", __func__, request->key, oracle_instance, ORA_DB_STATUS[1], ORA_DB_STATUS[oracle_db_status]);
			zbx_db_clean_result(&ora_dbstatus_result);

			if (oracle_db_status == ORA_ACTIVE)
				goto next;
			else if (oracle_db_status == ORA_ANY_STATUS)
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Oracle instance '%s' not found.", __func__, request->key, oracle_instance);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Oracle instance not found"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode.", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode."));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
			zbx_db_close_db(oracle_conn);
			zbx_db_clean_connection(oracle_conn);
			goto out;
		}
next:
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_ver_result, ORACLE_VERSION_DBS, oracle_instance))
		{
			ora_version = get_str_one_result(request, result, 0, 0, ora_ver_result);
			zbx_db_clean_result(&ora_ver_result);

			if (NULL != ora_version)
			{
				zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version: %s", __func__, request->key, ora_version);

				if (0 > zbx_strcmp_natural(ora_version, "12.0.0.0.0"))
				{
					query = ORACLE_V11_DB_INFO_DBS;
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version < 12, use query '%s'", __func__, request->key, query);
				}
				else
				{
					query = ORACLE_V12_DB_INFO_DBS;
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version >= 12, use query '%s'", __func__, request->key, query);
				}
			}
			else
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error get Oracle version in db_info procedure"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
			zbx_db_close_db(oracle_conn);
			zbx_db_clean_connection(oracle_conn);
			goto out;
		}

		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, query, oracle_instance, oracle_dbname))
		{
			ret = make_result(request, result, ora_result, ZBX_DB_RES_TYPE_ONEROW);
			zbx_db_clean_result(&ora_result);
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
		}

		zbx_db_close_db(oracle_conn);
		zbx_db_clean_connection(oracle_conn);
	}
	else
	{
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		if (NULL != conn_error)
			SET_MSG_RESULT(result, zbx_strdup(NULL, conn_error));
		else
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}

out:
	zbx_free(conn_error);

	return ret;
}

int	ORACLE_PDB_INFO(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int							ret = SYSINFO_RET_FAIL;
	char						*oracle_conn_string, *oracle_str_mode, *oracle_instance, *ora_version, *conn_error = NULL;
	unsigned short				oracle_mode = ZBX_DB_OCI_DEFAULT;
	unsigned int				oracle_db_status = 0;
	struct zbx_db_connection	*oracle_conn;
	struct zbx_db_result		ora_ver_result, ora_dbstatus_result, ora_result;
	const char					*query = ORACLE_V12_PDB_INFO_DBS;

	if (NULL == CONFIG_ORACLE_USER)
		CONFIG_ORACLE_USER = zbx_strdup(CONFIG_ORACLE_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PASSWORD)
		CONFIG_ORACLE_PASSWORD = zbx_strdup(CONFIG_ORACLE_PASSWORD, ORACLE_DEFAULT_PASSWORD);
	if (NULL == CONFIG_ORACLE_INSTANCE)
		CONFIG_ORACLE_INSTANCE = zbx_strdup(CONFIG_ORACLE_INSTANCE, ORACLE_DEFAULT_INSTANCE);

	if (3 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (3 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few options."));
		return SYSINFO_RET_FAIL;
	}

	oracle_conn_string = get_rparam(request, 0);
	oracle_instance = get_rparam(request, 1);
	oracle_str_mode = get_rparam(request, 2);

	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): request->nparam = %d", __func__, request->key, request->nparam);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[0] = %s", __func__, request->key, oracle_conn_string);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[1] = %s", __func__, request->key, oracle_instance);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[2] = %s", __func__, request->key, oracle_str_mode);

	if (NULL == oracle_conn_string || '\0' == *oracle_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (connection string)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_instance || '\0' == *oracle_instance)
	{
		oracle_instance = CONFIG_ORACLE_INSTANCE;
	}

	if (NULL == oracle_str_mode || '\0' == *oracle_str_mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_mode, &oracle_mode))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits."));
			return SYSINFO_RET_FAIL;
		}

		if (5 < oracle_mode)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits (0 - 5)."));
			return SYSINFO_RET_FAIL;
		}
	}

	if (1 == CONFIG_ORACLE_USE_LOCAL_ENV)
		oracle_conn_string = NULL;

	oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_USER, CONFIG_ORACLE_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);

	if (oracle_conn != NULL)
	{
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_dbstatus_result, ORACLE_CHECK_SUSPEND_MODE_DBS, oracle_instance))
		{
			oracle_db_status = get_int_one_result(request, result, 0, 0, ora_dbstatus_result);
			zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Need_DBStatus: %s, Current_DBStatus: %s", __func__, request->key, oracle_instance, ORA_DB_STATUS[1], ORA_DB_STATUS[oracle_db_status]);
			zbx_db_clean_result(&ora_dbstatus_result);

			if (oracle_db_status == ORA_ACTIVE)
				goto next;
			else if (oracle_db_status == ORA_ANY_STATUS)
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Oracle instance '%s' not found.", __func__, request->key, oracle_instance);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Oracle instance not found"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode.", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Database status not ACTIVE. Instance has SUSPENDED or INSTANCE RECOVERY mode."));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
			zbx_db_close_db(oracle_conn);
			zbx_db_clean_connection(oracle_conn);
			goto out;
		}
	next:
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_ver_result, ORACLE_VERSION_DBS, oracle_instance))
		{
			ora_version = get_str_one_result(request, result, 0, 0, ora_ver_result);
			zbx_db_clean_result(&ora_ver_result);

			if (NULL != ora_version)
			{
				zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version: %s", __func__, request->key, ora_version);

				if (0 > zbx_strcmp_natural(ora_version, "12.0.0.0.0"))
				{
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version < 12, pluggable database not supported", __func__, request->key);
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Oracle version < 12, pluggable database not supported"));
					ret = SYSINFO_RET_FAIL;
					zbx_db_close_db(oracle_conn);
					zbx_db_clean_connection(oracle_conn);
					goto out;
				}

				if (0 > zbx_strcmp_natural(ora_version, "18.0.0.0.0"))
				{
					query = ORACLE_V12_PDB_INFO_DBS;
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version < 18, use query '%s'", __func__, request->key, query);
				}
				else
				{
					query = ORACLE_V18_PDB_INFO_DBS;
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version >= 18, use query '%s'", __func__, request->key, query);
				}
			}
			else
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error get Oracle version in db_info procedure"));
				ret = SYSINFO_RET_FAIL;
				zbx_db_close_db(oracle_conn);
				zbx_db_clean_connection(oracle_conn);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
			zbx_db_close_db(oracle_conn);
			zbx_db_clean_connection(oracle_conn);
			goto out;
		}

		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, query, oracle_instance))
		{
			ret = make_result(request, result, ora_result, ZBX_DB_RES_TYPE_MULTIROW);
			zbx_db_clean_result(&ora_result);
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
			ret = SYSINFO_RET_FAIL;
		}

		zbx_db_close_db(oracle_conn);
		zbx_db_clean_connection(oracle_conn);
	}
	else
	{
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		if (NULL != conn_error)
			SET_MSG_RESULT(result, zbx_strdup(NULL, conn_error));
		else
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}

out:
	if (NULL != conn_error)
		zbx_free(conn_error);

	return ret;
}

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	oracle_ts_info(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	oracle_ts_info(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	int							ret = SYSINFO_RET_FAIL;
	char						*check_error = NULL, *oracle_conn_string, *oracle_str_mode, *oracle_instance, *ora_version, *oracle_str_ts_type, *conn_error = NULL;
	unsigned short				oracle_mode = ZBX_DB_OCI_DEFAULT, oracle_ts_type = ORA_TS_PERMANENT;
	unsigned int				oracle_db_open_mode = 0;
	struct zbx_db_connection	*oracle_conn;
	struct zbx_db_result		ora_result;
	char						*query = ORACLE_V11_PERMANENT_TS_INFO_DBS;

	if (NULL == CONFIG_ORACLE_USER)
		CONFIG_ORACLE_USER = zbx_strdup(CONFIG_ORACLE_USER, ORACLE_DEFAULT_USER);
	if (NULL == CONFIG_ORACLE_PASSWORD)
		CONFIG_ORACLE_PASSWORD = zbx_strdup(CONFIG_ORACLE_PASSWORD, ORACLE_DEFAULT_PASSWORD);
	if (NULL == CONFIG_ORACLE_INSTANCE)
		CONFIG_ORACLE_INSTANCE = zbx_strdup(CONFIG_ORACLE_INSTANCE, ORACLE_DEFAULT_INSTANCE);

	if (4 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (4 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few options."));
		return SYSINFO_RET_FAIL;
	}

	oracle_conn_string = get_rparam(request, 0);
	oracle_instance = get_rparam(request, 1);
	oracle_str_mode = get_rparam(request, 2);
	oracle_str_ts_type = get_rparam(request, 3);

	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): request->nparam = %d", __func__, request->key, request->nparam);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[0] = %s", __func__, request->key, oracle_conn_string);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[1] = %s", __func__, request->key, oracle_instance);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[2] = %s", __func__, request->key, oracle_str_mode);
	zabbix_log(LOG_LEVEL_TRACE, "Is %s(%s): nparam[3] = %s", __func__, request->key, oracle_str_ts_type);

	if (NULL == oracle_conn_string || '\0' == *oracle_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (connection string)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_instance || '\0' == *oracle_instance)
	{
		oracle_instance = CONFIG_ORACLE_INSTANCE;
	}

	if (FAIL == zbx_check_oracle_instance_name(oracle_instance, &check_error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "Invalid \"Instance name\" parameter: '%s': %s", oracle_instance, check_error);
		zbx_free(check_error);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (instance name)."));
		return SYSINFO_RET_FAIL;
	}

	if (NULL == oracle_str_mode || '\0' == *oracle_str_mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_mode, &oracle_mode))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits."));
			return SYSINFO_RET_FAIL;
		}

		if (5 < oracle_mode)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter (mode). Use only digits (0 - 5)."));
			return SYSINFO_RET_FAIL;
		}
	}

	if (NULL == oracle_str_ts_type || '\0' == *oracle_str_ts_type)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter (tstype)."));
		return SYSINFO_RET_FAIL;
	}
	else
	{
		if (SUCCEED != is_ushort(oracle_str_ts_type, &oracle_ts_type))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter (tstype). Use only digits (0 - 2)."));
			return SYSINFO_RET_FAIL;
		}

		if (2 < oracle_ts_type)
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Incorrect fourth parameter (tstype = %u). Allow: 0 - permanent, 1 - temporary, 2 - undo", __func__, request->key, oracle_ts_type);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Incorrect fourth parameter. Allow: 0 - permanent, 1 - temporary, 2 - undo"));
			return SYSINFO_RET_FAIL;
		}
	}

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the oracle_ts_info() prototype as required by */
	/* zbx_execute_dbmon_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	if (1 == CONFIG_ORACLE_USE_LOCAL_ENV)
		oracle_conn_string = NULL;

	oracle_conn = zbx_db_connect_oracle(oracle_conn_string, CONFIG_ORACLE_USER, CONFIG_ORACLE_PASSWORD, zbx_db_get_oracle_mode(oracle_mode), &conn_error);

	if (oracle_conn != NULL)
	{
		if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_VERSION_DBS, oracle_instance))
		{
			ora_version = get_str_one_result(request, result, 0, 0, ora_result);

			if (NULL != ora_version)
			{
				zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Oracle version: %s", __func__, request->key, ora_version);

				if (0 > zbx_strcmp_natural(ora_version, "12.0.0.0.0"))
				{
					switch ((zbx_db_oracle_ts_type)oracle_ts_type)
					{
						case ORA_TS_PERMANENT:
							query = ORACLE_V11_PERMANENT_TS_INFO_DBS;
							break;
						case ORA_TS_TEMPORARY:
							query = ORACLE_V11_TEMPORARY_TS_INFO_DBS;
							break;
						case ORA_TS_UNDO:
							query = ORACLE_V11_UNDO_TS_INFO_DBS;
							break;
						default:
							query = ORACLE_V11_PERMANENT_TS_INFO_DBS;
							break;
					}
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version < 12, use query '%s'", __func__, request->key, query);
				}
				else
				{
					switch (oracle_ts_type)
					{
						case ORA_TS_PERMANENT:
							query = ORACLE_V12_PERMANENT_TS_INFO_DBS;
							break;
						case ORA_TS_TEMPORARY:
							query = ORACLE_V12_TEMPORARY_TS_INFO_DBS;
							break;
						case ORA_TS_UNDO:
							query = ORACLE_V12_UNDO_TS_INFO_DBS;
							break;
						default:
							query = ORACLE_V12_PERMANENT_TS_INFO_DBS;
							break;
					}
					zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Oracle version >= 12, use query '%s'", __func__, request->key, query);
				}
			}

			zbx_db_clean_result(&ora_result);

			if (ZBX_DB_OK == zbx_db_query_select(oracle_conn, &ora_result, ORACLE_CHECK_INST_OPEN_MODE_WITHOUT_DBROLE_DBS, oracle_instance))
			{
				oracle_db_open_mode = get_int_one_result(request, result, 0, 0, ora_result);
				zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): Instance: %s, Open_Mode: %u (1 - MOUNTED, 2 - READ WRITE, 3 - READ ONLY, 4 - READ ONLY WITH APPLY)", __func__, request->key, oracle_instance, oracle_db_open_mode);
				zbx_db_clean_result(&ora_result);

				// 0 - UNKNOWN
				// 1 - MOUNTED
				// 2 - READ WRITE
				// 3 - READ ONLY
				// 4 - READ ONLY WITH APPLY
				if (2 <= oracle_db_open_mode)
				{
					ret = oracle_make_result(request, result, query, ZBX_DB_RES_TYPE_MULTIROW, ORA_ANY_ROLE, 0, ORA_ACTIVE);
				}
				else
				{
					zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s): Instance closed for reading information (may be database not open with read-write or read-only or read-only-with-apply)", __func__, request->key);
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Instance closed for reading information (may be database not open with read-write or read-only or read-only-with-apply)"));
					ret = SYSINFO_RET_FAIL;
				}
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query"));
				ret = SYSINFO_RET_FAIL;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error executing query", __func__, request->key);
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error executing query "));
			ret = SYSINFO_RET_FAIL;
		}

		zbx_db_close_db(oracle_conn);
		zbx_db_clean_connection(oracle_conn);
	}
	else
	{
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		if (NULL != conn_error)
			SET_MSG_RESULT(result, zbx_strdup(NULL, conn_error));
		else
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}

	zbx_free(conn_error);

	return ret;
}

int	ORACLE_TS_INFO(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	return zbx_execute_dbmon_threaded_metric(oracle_ts_info, request, result);
}

/*
 * Custom key
 * oracle.query.nojson[]
 * oracle.query.onerow[]
 * oracle.query.twocoll[]
 * oracle.query.multirow[]
 * oracle.query.discovery[]
 *
 * Returns the value of the specified SQL query.
 *
 * Parameters:
 *   0:  oracle connections string
 *   1:  oracle instance name string
 *   2:  oracle connection mode string
 *   3:  oracle database name string
 *   4:  name SQL query to execute
 *   n:  query parameters
 *
 * Returns: string
 *
 */
int	ORACLE_QUERY(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int					ret = SYSINFO_RET_FAIL;
	const char			*query_key = NULL, *query = NULL;
	zbx_db_result_type	query_result_type = ZBX_DB_RES_TYPE_NOJSON;
	int					i = 0;
	DBMONparams			params = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (5 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (5 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (0 == strcmp(request->key, "oracle.query.nojson"))
	{
		query_result_type = ZBX_DB_RES_TYPE_NOJSON;
	}
	else if (0 == strcmp(request->key, "oracle.query.onerow"))
	{
		query_result_type = ZBX_DB_RES_TYPE_ONEROW;
	}
	else if (0 == strcmp(request->key, "oracle.query.twocoll"))
	{
		query_result_type = ZBX_DB_RES_TYPE_TWOCOLL;
	}
	else if (0 == strcmp(request->key, "oracle.query.multirow"))
	{
		query_result_type = ZBX_DB_RES_TYPE_MULTIROW;
	}
	else if (0 == strcmp(request->key, "oracle.query.discovery"))
	{
		query_result_type = ZBX_DB_RES_TYPE_DISCOVERY;
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown request key."));
		ret = SYSINFO_RET_FAIL;
		goto out;
	}

	// Get the user SQL query parameter
	query_key = get_rparam(request, 4);

	if (strisnull(query_key))
	{
		dbmon_log_result(result, LOG_LEVEL_ERR, "No query or query-key specified.");
		goto out;
	}

	if (0 != init_dbmon_config_done)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Error init dbmon sql config file."));
		ret = SYSINFO_RET_FAIL;
		goto out;
	}

	// Check if query comes from configs
	query = get_query_by_name(query_key);

	if (NULL == query)
	{
		dbmon_log_result(result, LOG_LEVEL_DEBUG, "No query found for '%s'.", query_key);
		goto out;
		//query = query_key;
	}

	if (5 < request->nparam)
	{
		// parse user params
		dbmon_log_result(result, LOG_LEVEL_DEBUG, "Appending %i params to query.", request->nparam - 5);

		for (i = 5; i < request->nparam; i++)
		{
			params = dbmon_param_append(params, get_rparam(request, i));
		}
	}

	dbmon_log_result(result, LOG_LEVEL_TRACE, "Execute query: %s", query);

	ret = oracle_make_result(request, result, query, query_result_type, ORA_ANY_ROLE, 0, ORA_ANY_STATUS);

	if (5 < request->nparam)
		dbmon_param_free(params);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s): %s", __func__, request->key, zbx_sysinfo_ret_string(ret));

	return ret;
}
#endif
#endif