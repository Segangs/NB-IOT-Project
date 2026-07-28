begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_count bigint;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if to_regprocedure(
           'public.capture_device_boot_alert()'
       ) is not null
       or exists (
           select 1
           from pg_catalog.pg_trigger as t
           where t.tgrelid = 'public.device_boot_logs'::regclass
             and t.tgname = 'trg_capture_device_boot_alert'
             and not t.tgisinternal
       ) then
        raise exception 'device boot alert target object already exists';
    end if;

    if to_regclass('public.device_boot_logs') is null
       or to_regclass('public.device') is null
       or to_regclass('public.userworkplace') is null
       or to_regclass('public.usermachine') is null
       or to_regclass('public.alert_contact') is null
       or to_regclass('public.message_policy') is null
       or to_regclass('public.msg_send') is null
       or to_regclass('public.message_link_token') is null then
        raise exception 'required device boot alert relation is missing';
    end if;

    if not exists (
        select 1 from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device_boot_logs'
          and column_name = 'id'
          and data_type = 'bigint'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device_boot_logs'
          and column_name = 'boottime'
          and data_type = 'character varying'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device_boot_logs'
          and column_name = 'userId'
          and data_type = 'integer'
    ) or not exists (
        select 1 from information_schema.columns
        where table_schema = 'public'
          and table_name = 'device_boot_logs'
          and column_name = 'deviceId'
          and data_type = 'integer'
    ) then
        raise exception 'device boot log identity contract drift';
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
