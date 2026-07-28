begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_count bigint;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if to_regclass('public.device_power_event') is not null
       or to_regprocedure(
           'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)'
       ) is not null
       or exists (
           select 1
           from vault.decrypted_secrets as s
           where s.name = 'nb_iot_event_gateway_secret'
       ) then
        raise exception 'power event target object already exists';
    end if;

    if to_regclass('public.device') is null
       or to_regclass('public.userworkplace') is null
       or to_regclass('public.usermachine') is null
       or to_regclass('public.alert_contact') is null
       or to_regclass('public.message_policy') is null
       or to_regclass('public.msg_send') is null
       or to_regclass('public.message_link_token') is null then
        raise exception 'required power event relation is missing';
    end if;

    if not exists (
        select 1
        from vault.decrypted_secrets as s
        where s.name = 'nb_iot_config_request_secret'
          and nullif(s.decrypted_secret, '') is not null
    ) then
        raise exception 'power event source secret is unavailable';
    end if;

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device'
          and column_name = 'deviceId'
          and data_type = 'integer'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device'
          and column_name = 'deviceIMEI'
          and data_type = 'character varying'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device'
          and column_name = 'userId'
          and data_type = 'integer'
    ) then
        raise exception 'power event device identity contract drift';
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
