@echo off

title Update zabbix-agent...

set service_name="Zabbix Agent [MYHOMEPC]"
set src_dir="D:\Project\zabbix_dbmon_github\bin\win64"

:stop
echo Stoping service %service_name%...
sc stop %service_name%>nul

rem cause a ~3 second sleep before checking the service state
echo Wait 3 sec...
ping 127.0.0.1 -n 3 -w 1000 > nul

sc query %service_name% | find /I "STATE" | find "STOPPED">nul
if errorlevel 1 goto :stop
goto :copy

:copy
echo Copy new version of zabbix_agentd.exe...
copy /Y %src_dir%\zabbix_agentd.exe C:\DBS_Zabbix>nul
if errorlevel 1 goto :error
goto :start

:start
echo Starting service %service_name%...
net start | find /i %service_name%>nul && goto :start
sc start %service_name%>nul
goto :end

:error
echo ERROR: Failed to update zabbix_agentd.exe
goto :start

:end