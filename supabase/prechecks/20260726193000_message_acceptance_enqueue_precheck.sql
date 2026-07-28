begin read only;

set local statement_timeout = '30s';

do $precheck$
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_roles as r
        where r.rolname = 'service_role'
    ) then
        raise exception 'required service_role is missing';
    end if;

    if to_regclass('public.msg_send') is null
       or to_regclass('public.message_link_token') is null
       or to_regclass('public.alert_contact') is null
       or to_regclass('public.message_policy') is null
       or to_regclass('public.device') is null
       or to_regclass('public.userworkplace') is null
       or to_regclass('public."USER_SENSOR"') is null
       or to_regclass('public."SENSOR_CTGY"') is null then
        raise exception 'required message or ownership relation is missing';
    end if;

    if to_regprocedure(
           'public.mark_msg_send_submission_waiting_result(bigint,text,uuid,text,text,timestamptz)'
       ) is null
       or to_regprocedure(
           'public.record_msg_send_push_result(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,text,integer,integer,integer,integer)'
       ) is null then
        raise exception 'required message transition function is missing';
    end if;

    if to_regprocedure('extensions.digest(text,text)') is null then
        raise exception 'required extensions.digest(text,text) is missing';
    end if;

    if to_regprocedure(
           'public.finalize_msg_send_provider_acceptance(bigint,text,uuid,text,text,timestamptz,numeric)'
       ) is not null
       or to_regprocedure(
           'public.enqueue_temp_alert_msg_send(bigint,bigint,integer,integer,text,bigint,bigint,text,jsonb,text,timestamptz,smallint)'
       ) is not null then
        raise exception 'one or more target functions already exist';
    end if;

    if not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'device'
          and c.column_name = 'deviceId'
    )
       or not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'device'
          and c.column_name = 'userId'
    )
       or not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'device'
          and c.column_name = 'userWorkplaceId'
    )
       or not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'USER_SENSOR'
          and c.column_name in ('Id', 'deviceId', 'sensorCtgyId')
        group by c.table_schema, c.table_name
        having pg_catalog.count(*) = 3
    )
       or not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'SENSOR_CTGY'
          and c.column_name in ('sensorCtgyId', 'sensorCtgyType')
        group by c.table_schema, c.table_name
        having pg_catalog.count(*) = 2
    ) then
        raise exception 'ownership or TEMP sensor column contract drift';
    end if;
end;
$precheck$;

rollback;
