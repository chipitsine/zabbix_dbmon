/*
** Zabbix
** Copyright (C) 2019 Mikhail Grigorev
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
#include "pgsql_info.h"
#include "dbmon_common.h"

#if defined(HAVE_DBMON)
#if defined(HAVE_POSTGRESQL)

#define PGSQL_VERSION_DBS "SELECT VERSION() AS VERSION;"

#define PGSQL_SERVER_INFO_DBS "\
SELECT  VERSION() AS VERSION, \
	date_part('epoch', pg_postmaster_start_time())::int AS STARTUPTIME, \
	date_part('epoch', now() - pg_postmaster_start_time())::int AS UPTIME;"

#define PGSQL_DB_DISCOVERY_DBS  "\
SELECT \
    d.oid AS OID, \
    d.datname AS DBNAME, \
    pg_catalog.pg_encoding_to_char(d.encoding) AS ENCODING, \
    d.datcollate AS LC_COLLATE, \
    d.datctype AS LC_CTYPE, \
    pg_catalog.pg_get_userbyid(d.datdba) AS OWNER, \
    t.spcname AS TABLESPACE, \
    pg_catalog.shobj_description(d.oid, 'pg_database') AS DESCRIPTION, \
	age(d.datfrozenxid) AS AGE, \
	d.datconnlimit AS CONNLIM \
FROM pg_catalog.pg_database d \
    JOIN pg_catalog.pg_tablespace t on d.dattablespace = t.oid \
WHERE \
    d.datallowconn = 't' \
    AND d.datistemplate = 'n' \
ORDER BY 1;"

#define PGSQL_DB_INFO_DBS "\
SELECT \
	d.oid AS oid, \
	d.datname AS DBNAME, \
	pg_catalog.pg_encoding_to_char(d.encoding) AS ENCODING, \
	d.datcollate AS LC_COLLATE, \
	d.datctype AS LC_CTYPE, \
	pg_catalog.pg_get_userbyid(d.datdba) AS OWNER, \
	t.spcname AS TABLESPACE, \
	pg_catalog.shobj_description(d.oid, 'pg_database') AS DESCRIPTION, \
	age(d.datfrozenxid) AS AGE, \
	d.datconnlimit AS CONNLIM, \
	pg_database_size(d.datname::text) AS DBSIZE \
FROM pg_catalog.pg_database d \
	JOIN pg_catalog.pg_tablespace t on d.dattablespace = t.oid \
WHERE \
	d.datallowconn = 't' \
	AND d.datistemplate = 'n' \
ORDER BY 1;"

#define PGSQL_DB_LOCKS_DBS "\
WITH T AS \
(SELECT db.datname AS DBNAME, \
	lower(replace(Q.mode, 'Lock', '')) AS MODE, \
	coalesce(T.qty, 0) val \
	FROM pg_database db \
	JOIN( \
		VALUES('AccessShareLock'), ('RowShareLock'), ('RowExclusiveLock'), ('ShareUpdateExclusiveLock'), ('ShareLock'), ('ShareRowExclusiveLock'), ('ExclusiveLock'), ('AccessExclusiveLock')) Q(MODE) ON TRUE NATURAL \
	LEFT JOIN \
	(SELECT datname, \
		MODE, \
		count(MODE) qty \
		FROM pg_locks lc \
		RIGHT JOIN pg_database db ON db.oid = lc.database \
		GROUP BY 1, 2) T \
	WHERE NOT db.datistemplate \
	ORDER BY 1, 2) \
	SELECT json_object_agg(dbname, row_to_json(T2)) \
	FROM \
	(SELECT dbname, \
		sum(val) AS TOTAL, \
		sum(CASE \
			WHEN MODE = 'accessexclusive' THEN val \
			END) AS ACCESSEXCLUSIVE, \
		sum(CASE \
			WHEN MODE = 'accessshare' THEN val \
			END) AS ACCESSSHARE, \
		sum(CASE \
			WHEN MODE = 'exclusive' THEN val \
			END) AS EXCLUSIVE, \
		sum(CASE \
			WHEN MODE = 'rowexclusive' THEN val \
			END) AS ROWEXCLUSIVE, \
		sum(CASE \
			WHEN MODE = 'rowshare' THEN val \
			END) AS ROWSHARE, \
		sum(CASE \
			WHEN MODE = 'share' THEN val \
			END) AS SHARE, \
		sum(CASE \
			WHEN MODE = 'sharerowexclusive' THEN val \
			END) AS SHAREROWEXCLUSIVE, \
		sum(CASE \
			WHEN MODE = 'shareupdateexclusive' THEN val \
			END) AS SHAREUPDATEEXCLUSIVE \
		FROM T \
		GROUP BY dbname) T2"

#define PGSQL_DB_STAT_SUM_DBS "\
SELECT row_to_json(T) \
FROM( \
	SELECT \
	sum(numbackends) as numbackends \
	, sum(xact_commit) as xact_commit \
	, sum(xact_rollback) as xact_rollback \
	, sum(blks_read) as blks_read \
	, sum(blks_hit) as blks_hit \
	, sum(tup_returned) as tup_returned \
	, sum(tup_fetched) as tup_fetched \
	, sum(tup_inserted) as tup_inserted \
	, sum(tup_updated) as tup_updated \
	, sum(tup_deleted) as tup_deleted \
	, sum(conflicts) as conflicts \
	, sum(temp_files) as temp_files \
	, sum(temp_bytes) as temp_bytes \
	, sum(deadlocks) as deadlocks \
	, %s as checksum_failures \
	, sum(blk_read_time) as blk_read_time \
	, sum(blk_write_time) as blk_write_time \
	FROM pg_catalog.pg_stat_database \
) T;"

#define PGSQL_DB_STAT_DBS "\
SELECT json_object_agg(coalesce(datname, 'null'), row_to_json(T)) \
FROM( \
	SELECT \
	datname \
	, numbackends as numbackends \
	, xact_commit as xact_commit \
	, xact_rollback as xact_rollback \
	, blks_read as blks_read \
	, blks_hit as blks_hit \
	, tup_returned as tup_returned \
	, tup_fetched as tup_fetched \
	, tup_inserted as tup_inserted \
	, tup_updated as tup_updated \
	, tup_deleted as tup_deleted \
	, conflicts as conflicts \
	, temp_files as temp_files \
	, temp_bytes as temp_bytes \
	, deadlocks as deadlocks \
	, %s as checksum_failures \
	, blk_read_time as blk_read_time \
	, blk_write_time as blk_write_time \
	FROM pg_catalog.pg_stat_database \
) T;"

ZBX_METRIC	parameters_dbmon_pgsql[] =
/*	KEY			FLAG		FUNCTION		TEST PARAMETERS */
{
	{"pgsql.ping",			CF_HAVEPARAMS,		PGSQL_PING,			NULL},
	{"pgsql.version",		CF_HAVEPARAMS,		PGSQL_VERSION,		NULL},
	{"pgsql.version.full",	CF_HAVEPARAMS,		PGSQL_VERSION,		NULL},
	{"pgsql.server.info",	CF_HAVEPARAMS,		PGSQL_GET_RESULT,	NULL},
	{"pgsql.db.discovery",	CF_HAVEPARAMS,		PGSQL_DB_DISCOVERY,	NULL},
	{"pgsql.db.info",		CF_HAVEPARAMS,		PGSQL_GET_RESULT,	NULL},
	{"pgsql.db.locks",		CF_HAVEPARAMS,		PGSQL_GET_RESULT,	NULL},
	{"pgsql.db.stat.sum",	CF_HAVEPARAMS,		PGSQL_GET_RESULT,	NULL},
	{"pgsql.db.stat",		CF_HAVEPARAMS,		PGSQL_GET_RESULT,	NULL},
	{NULL}
};

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	pgsql_ping(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	pgsql_ping(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	int							ret = SYSINFO_RET_FAIL, ping = 0;
	char						*pg_conn_string;
	struct zbx_db_connection	*pgsql_conn;

	if (1 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (1 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few parameters."));
		return SYSINFO_RET_FAIL;
	}

	pg_conn_string = get_rparam(request, 0);

	if (NULL == pg_conn_string || '\0' == *pg_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (pg_conn_string)."));
		return SYSINFO_RET_FAIL;
	}

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the pgsql_ping() prototype as required by */
	/* zbx_execute_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	pgsql_conn = zbx_db_connect_pgsql(pg_conn_string);

	if (NULL != pgsql_conn)
	{
		SET_UI64_RESULT(result, 1);
	}
	else
	{
		SET_UI64_RESULT(result, 0);
	}

	zbx_db_close_db(pgsql_conn);
	zbx_db_clean_connection(pgsql_conn);

	return SYSINFO_RET_OK;
}

int	PGSQL_PING(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	return zbx_execute_threaded_metric(pgsql_ping, request, result);
}

static int	pgsql_version(AGENT_REQUEST *request, AGENT_RESULT *result, unsigned int ver_mode)
{
	int							ret = SYSINFO_RET_FAIL;
	char						*pg_conn_string;
	struct zbx_db_connection	*pgsql_conn;
	struct zbx_db_result		pgsql_result;
	char						pgsql_ver[10];
	unsigned long				version;

	if (1 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (1 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few parameters."));
		return SYSINFO_RET_FAIL;
	}

	pg_conn_string = get_rparam(request, 0);

	if (NULL == pg_conn_string || '\0' == *pg_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (pg_conn_string)."));
		return SYSINFO_RET_FAIL;
	}

	pgsql_conn = zbx_db_connect_pgsql(pg_conn_string);

	if (NULL != pgsql_conn)
	{
		if (0 == ver_mode)
		{
			/*
			version 9.5.2	=> 90502
			version 9.6.0	=> 90600
			version 9.6.16	=> 90616
			version 10.5	=> 100500
			version 10.11	=> 100011
			version 12.1	=> 120001
			*/
			version = zbx_db_version(pgsql_conn);

			if (0 != version)
			{
				if (version < 100000)
				{
					zbx_snprintf(pgsql_ver, sizeof(pgsql_ver), "%lu.%lu.%lu", version / 10000, (version % 10000) / 100, (version % 10000) % 100);
				}
				else
				{
					zbx_snprintf(pgsql_ver, sizeof(pgsql_ver), "%lu.%lu.%lu", version / 10000, ((version % 10000) / 100) == 0 ? (version % 10000) % 100 : (version % 10000) / 100, ((version % 10000) / 100) == 0 ? (version % 10000) / 100 : (version % 10000) % 100);
				}

				zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): PgSQL version: %s", __func__, request->key, pgsql_ver);
				SET_TEXT_RESULT(result, zbx_strdup(NULL, pgsql_ver));
				ret = SYSINFO_RET_OK;
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error get PgSQL version", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error get PgSQL version"));
				ret = SYSINFO_RET_FAIL;
			}
		}
		else
		{
			if (ZBX_DB_OK == zbx_db_query_select(pgsql_conn, &pgsql_result, PGSQL_VERSION_DBS))
			{
				ret = make_result(request, result, pgsql_result, ZBX_DB_RES_TYPE_NOJSON);
				zbx_db_clean_result(&pgsql_result);
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
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}

	zbx_db_close_db(pgsql_conn);
	zbx_db_clean_connection(pgsql_conn);

	return ret;
}

int	PGSQL_VERSION(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (0 == strcmp(request->key, "pgsql.version"))
	{
		ret = pgsql_version(request, result, 0);
	}
	else if (0 == strcmp(request->key, "pgsql.version.full"))
	{
		ret = pgsql_version(request, result, 1);
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown request key"));
		ret = SYSINFO_RET_FAIL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s): %s", __func__, request->key, zbx_sysinfo_ret_string(ret));

	return ret;
}

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	pgsql_get_discovery(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	pgsql_get_discovery(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	int							ret = SYSINFO_RET_FAIL;
	char						*pgsql_conn_string, *c = NULL;
	struct zbx_db_connection	*pgsql_conn;
	struct zbx_db_result		pgsql_result;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (1 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (1 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few parameters."));
		return SYSINFO_RET_FAIL;
	}

	pgsql_conn_string = get_rparam(request, 0);

	if (NULL == pgsql_conn_string || '\0' == *pgsql_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (pg_conn_string)."));
		return SYSINFO_RET_FAIL;
	}

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the pgsql_get_discovery() prototype as required by */
	/* zbx_execute_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	pgsql_conn = zbx_db_connect_pgsql(pgsql_conn_string);

	if (NULL != pgsql_conn)
	{
		if (0 == strcmp(request->key, "pgsql.db.discovery"))
		{
			ret = zbx_db_query_select(pgsql_conn, &pgsql_result, PGSQL_DB_DISCOVERY_DBS);
		}
		else
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown discovery request key."));
			ret = SYSINFO_RET_FAIL;
			goto out;
		}

		if (ZBX_DB_OK == ret)
		{
			ret = make_result(request, result, pgsql_result, ZBX_DB_RES_TYPE_DISCOVERY);
			zbx_db_clean_result(&pgsql_result);
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
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}
out:
	zbx_db_close_db(pgsql_conn);
	zbx_db_clean_connection(pgsql_conn);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s): %s", __func__, request->key, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	PGSQL_DB_DISCOVERY(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	return zbx_execute_threaded_metric(pgsql_get_discovery, request, result);
	//return pgsql_get_discovery(request, result, NULL);
}

static int	pgsql_make_result(AGENT_REQUEST *request, AGENT_RESULT *result, char *query, zbx_db_result_type result_type)
{
	int							ret = SYSINFO_RET_FAIL, db_ret = ZBX_DB_ERROR;
	char						*pg_conn_string;
	struct zbx_db_connection	*pgsql_conn;
	struct zbx_db_result		pgsql_result;
	unsigned long				version;
	const char					*pg_db_sum;

	if (1 < request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too many parameters."));
		return SYSINFO_RET_FAIL;
	}

	if (1 > request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too few parameters."));
		return SYSINFO_RET_FAIL;
	}

	pg_conn_string = get_rparam(request, 0);

	if (NULL == pg_conn_string || '\0' == *pg_conn_string)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid first parameter (pg_conn_string)."));
		return SYSINFO_RET_FAIL;
	}

	pgsql_conn = zbx_db_connect_pgsql(pg_conn_string);

	if (NULL != pgsql_conn)
	{
		if (0 == strcmp(request->key, "pgsql.db.stat.sum"))
		{
			version = zbx_db_version(pgsql_conn);

			if (0 != version)
			{
				zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): PgSQL version: %lu", __func__, request->key, version);

				if (version >= 120000)
				{
					pg_db_sum = "sum(checksum_failures)";
				}
				else
				{
					pg_db_sum = "null";
				}

				db_ret = zbx_db_query_select(pgsql_conn, &pgsql_result, query, pg_db_sum);
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error get PgSQL version", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error get PgSQL version"));
				ret = SYSINFO_RET_FAIL;
				goto out;
			}
		}
		else if (0 == strcmp(request->key, "pgsql.db.stat"))
		{
			version = zbx_db_version(pgsql_conn);

			if (0 != version)
			{
				zabbix_log(LOG_LEVEL_TRACE, "In %s(%s): PgSQL version: %lu", __func__, request->key, version);

				if (version >= 120000)
				{
					pg_db_sum = "checksum_failures";
				}
				else
				{
					pg_db_sum = "null";
				}

				db_ret = zbx_db_query_select(pgsql_conn, &pgsql_result, query, pg_db_sum);
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error get PgSQL version", __func__, request->key);
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Error get PgSQL version"));
				ret = SYSINFO_RET_FAIL;
				goto out;
			}
		}
		else
		{
			db_ret = zbx_db_query_select(pgsql_conn, &pgsql_result, query);
		}

		if (ZBX_DB_OK == db_ret)
		{
			ret = make_result(request, result, pgsql_result, result_type);
			zbx_db_clean_result(&pgsql_result);
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
		zabbix_log(LOG_LEVEL_WARNING, "In %s(%s): Error connecting to database", __func__, request->key);
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Error connecting to database"));
		ret = SYSINFO_RET_FAIL;
	}
out:
	zbx_db_close_db(pgsql_conn);
	zbx_db_clean_connection(pgsql_conn);

	return ret;
}

#if !defined(_WINDOWS) && !defined(__MINGW32__)
static int	pgsql_get_result(AGENT_REQUEST *request, AGENT_RESULT *result)
#else
static int	pgsql_get_result(AGENT_REQUEST *request, AGENT_RESULT *result, HANDLE timeout_event)
#endif
{
	int ret = SYSINFO_RET_FAIL;

#if defined(_WINDOWS) && defined(__MINGW32__)
	/* 'timeout_event' argument is here to make the pgsql_get_result() prototype as required by */
	/* zbx_execute_threaded_metric() on MS Windows */
	ZBX_UNUSED(timeout_event);
#endif

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(%s)", __func__, request->key);

	if (0 == strcmp(request->key, "pgsql.server.info"))
	{
		ret = pgsql_make_result(request, result, PGSQL_SERVER_INFO_DBS, ZBX_DB_RES_TYPE_ONEROW);
	}
	else if (0 == strcmp(request->key, "pgsql.db.info"))
	{
		ret = pgsql_make_result(request, result, PGSQL_DB_INFO_DBS, ZBX_DB_RES_TYPE_MULTIROW);
	}
	else if (0 == strcmp(request->key, "pgsql.db.locks"))
	{
		ret = pgsql_make_result(request, result, PGSQL_DB_LOCKS_DBS, ZBX_DB_RES_TYPE_NOJSON);
	}
	else if (0 == strcmp(request->key, "pgsql.db.stat.sum"))
	{
		ret = pgsql_make_result(request, result, PGSQL_DB_STAT_SUM_DBS, ZBX_DB_RES_TYPE_NOJSON);
	}
	else if (0 == strcmp(request->key, "pgsql.db.stat"))
	{
		ret = pgsql_make_result(request, result, PGSQL_DB_STAT_DBS, ZBX_DB_RES_TYPE_NOJSON);
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown request key"));
		ret = SYSINFO_RET_FAIL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(%s): %s", __func__, request->key, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	PGSQL_GET_RESULT(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	return zbx_execute_threaded_metric(pgsql_get_result, request, result);
}
#endif
#endif