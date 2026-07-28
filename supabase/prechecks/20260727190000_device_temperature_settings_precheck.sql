begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_definition text;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if to_regclass('public."USER_SENSOR"') is null
       or to_regclass('public."SENSOR_CTGY"') is null
       or to_regclass('public.sensorvalue') is null
       or to_regclass('public.device') is null
       or to_regclass('public.userworkplace') is null
       or to_regclass('public.temperature_alert_state') is null
       or to_regclass('public.temperature_alert_outbox') is null
       or to_regprocedure(
          'public.capture_temperature_alert_transition()'
       ) is null then
        raise exception 'base temperature repeat contract is missing';
    end if;

    if to_regclass(
           'public.temperature_alert_preference'
       ) is not null
       or to_regprocedure(
          'public.get_device_temperature_settings(integer,integer,integer)'
       ) is not null
       or to_regprocedure(
          'public.update_device_temperature_settings(integer,integer,integer,jsonb)'
       ) is not null then
        raise exception 'target settings object already exists';
    end if;

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'USER_SENSOR'
          and column_name = 'Id'
          and data_type = 'integer'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'USER_SENSOR'
          and column_name = 'userSensorId'
          and data_type = 'integer'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'USER_SENSOR'
          and column_name = 'deviceId'
          and data_type = 'integer'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'USER_SENSOR'
          and column_name = 'setTmpUpLimit'
          and data_type = 'real'
    ) then
        raise exception 'legacy temperature settings column drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'::regprocedure
    )
    into v_definition;
    if v_definition not ilike
           '%v_state.high_notification_count < 3%'
       or v_definition not ilike
           '%interval ''20 minutes''%'
       or v_definition ilike '%v_max_notifications%' then
        raise exception 'base repeat trigger definition drift';
    end if;

    if exists (
        select 1
        from public.temperature_alert_outbox
        where status = 'processing'
    ) then
        raise exception 'processing outbox must be zero';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid =
              'public.capture_temperature_alert_transition()'::regprocedure
          and (
              not p.prosecdef
              or p.provolatile <> 'v'
              or not exists (
                  select 1
                  from pg_catalog.unnest(p.proconfig) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
          )
    ) then
        raise exception 'base repeat trigger hardening drift';
    end if;
end;
$precheck$;

select
    (
        select pg_catalog.count(*)
        from public."USER_SENSOR" as s
        join public."SENSOR_CTGY" as c
          on c."sensorCtgyId" = s."sensorCtgyId"
        where c."sensorCtgyType" = 'TMP'
    ) as temperature_sensor_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_state
        where is_high
    ) as active_high_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where status = 'processing'
    ) as processing_outbox_count;

rollback;
