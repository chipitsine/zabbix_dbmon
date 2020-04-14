CREATE PROFILE ZABBIXMON_UNLIMITED_PWD LIMIT PASSWORD_LIFE_TIME UNLIMITED;

CREATE USER zabbixmon IDENTIFIED BY "zabbixmon" DEFAULT TABLESPACE SYSTEM TEMPORARY TABLESPACE TEMP PROFILE ZABBIXMON_UNLIMITED_PWD ACCOUNT UNLOCK;
GRANT CONNECT TO zabbixmon;
GRANT RESOURCE TO zabbixmon;
ALTER USER zabbixmon DEFAULT ROLE ALL;
GRANT CREATE SESSION TO zabbixmon;
GRANT SELECT ANY DICTIONARY TO zabbixmon;
GRANT UNLIMITED TABLESPACE TO zabbixmon;
GRANT SELECT ANY DICTIONARY TO zabbixmon;
GRANT SELECT ON V_$SESSION TO zabbixmon;
GRANT SELECT ON V_$SYSTEM_EVENT TO zabbixmon;
GRANT SELECT ON V_$EVENT_NAME TO zabbixmon;
GRANT SELECT ON V_$RECOVERY_FILE_DEST TO zabbixmon;
GRANT SELECT ON V_$ARCHIVED_LOG TO zabbixmon;
GRANT SELECT ON V_$DATABASE TO zabbixmon;
GRANT SELECT ON V_$INSTANCE TO zabbixmon;
GRANT SELECT ON V_$ARCHIVE_GAP TO zabbixmon;
GRANT SELECT ON V_$MANAGED_STANDBY TO zabbixmon;

GRANT SELECT ANY TABLE TO zabbixmon;
GRANT sysdba to zabbixmon;
