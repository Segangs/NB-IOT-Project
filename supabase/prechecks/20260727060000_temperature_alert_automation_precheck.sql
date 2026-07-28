begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_count bigint;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;
    if to_regclass('public.temperature_alert_state') is not null
       or to_regclass('public.temperature_alert_outbox') is not null
       or to_regclass('public.temperature_alert_incident_id_seq') is not null
       or to_regprocedure(
          'public.capture_temperature_alert_transition()'
       ) is not null
       or to_regprocedure(
          'public.drain_temperature_alert_outbox(integer)'
       ) is not null then
        raise exception 'temperature alert target object already exists';
    end if;
    if to_regprocedure(
        'public.enqueue_temp_alert_msg_send(bigint,bigint,integer,integer,text,bigint,bigint,text,jsonb,text,timestamp with time zone,smallint)'
    ) is null then
        raise exception 'required enqueue_temp_alert_msg_send is missing';
    end if;
    if exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_capture_temperature_alert_transition'
          and not t.tgisinternal
    ) then
        raise exception 'target temperature trigger already exists';
    end if;
    if to_regclass('public.sensorvalue') is null
       or to_regclass('public."USER_SENSOR"') is null
       or to_regclass('public."SENSOR_CTGY"') is null
       or to_regclass('public.device') is null
       or to_regclass('public.userworkplace') is null
       or to_regclass('public.usermachine') is null
       or to_regclass('public.alert_contact') is null
       or to_regclass('public.message_policy') is null then
        raise exception 'required temperature alert relation is missing';
    end if;

    if not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'sensorvalue'
          and column_name = 'sensorValueId' and data_type = 'integer'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'sensorvalue'
          and column_name = 'sensorId' and data_type = 'integer'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'sensorvalue'
          and column_name = 'sensorValue' and data_type = 'real'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'sensorvalue'
          and column_name = 'sensorvaluetime'
          and data_type = 'timestamp without time zone'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'USER_SENSOR'
          and column_name = 'Id' and data_type = 'integer'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'USER_SENSOR'
          and column_name = 'deviceId' and data_type = 'integer'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'USER_SENSOR'
          and column_name = 'sensorCtgyId' and data_type = 'integer'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public' and table_name = 'USER_SENSOR'
          and column_name = 'setTmpUpLimit' and data_type = 'real'
    ) then
        raise exception 'legacy temperature column contract drift';
    end if;

    select pg_catalog.count(*)
    into v_count
    from public.alert_contact as c
    where c.is_active
      and c.consent_status = 'granted'
      and c.consent_revoked_at is null;
    if v_count < 1 then
        raise exception 'expected at least one active consented contact';
    end if;

    select pg_catalog.count(*)
    into v_count
    from public.message_policy as p
    where p.is_active
      and p.primary_channel = 'alimtalk'
      and not p.allow_sms_fallback
      and p.effective_from <= pg_catalog.clock_timestamp()
      and (
          p.effective_until is null
          or p.effective_until > pg_catalog.clock_timestamp()
      );
    if v_count <> 1 then
        raise exception 'expected exactly one active Alimtalk-only policy';
    end if;
end;
$precheck$;

rollback;
