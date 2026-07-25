begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_assign_md5 text;
    v_invalid_count bigint;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if current_setting('TimeZone') <> 'Asia/Seoul' then
        raise exception 'expected database TimeZone Asia/Seoul, got %',
            current_setting('TimeZone');
    end if;

    if to_regclass('public."deviceCmds"') is null
       or to_regclass('public.device') is null then
        raise exception 'legacy public."deviceCmds" or public.device is missing';
    end if;

    if to_regclass('public.device_command_state') is not null
       or to_regprocedure('public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)') is not null
       or to_regprocedure('public.sync_device_command_state()') is not null
       or to_regprocedure('public.claim_device_command(text,bigint,bigint,integer)') is not null
       or to_regprocedure('public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)') is not null then
        raise exception 'one or more target objects already exist';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public."deviceCmds"'::regclass
          and t.tgname = 'trg_sync_device_command_state'
          and not t.tgisinternal
    ) then
        raise exception 'target trigger trg_sync_device_command_state already exists';
    end if;

    if to_regprocedure('public.assign_device_command()') is null then
        raise exception 'observed legacy function assign_device_command() is missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_assign_device_command'
          and not t.tgisinternal
          and t.tgenabled = 'O'
          and t.tgfoid = 'public.assign_device_command()'::regprocedure
    ) then
        raise exception 'observed legacy trigger trg_assign_device_command definition drift';
    end if;

    select md5(pg_get_functiondef('public.assign_device_command()'::regprocedure))
    into v_assign_md5;

    if v_assign_md5 <> 'de97b37ad246c5e509ae06f821a10338' then
        raise exception 'legacy assign_device_command() definition drift';
    end if;

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'deviceCmds'
          and column_name = 'cmdId'
          and data_type = 'integer'
          and is_nullable = 'NO'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'deviceCmds'
          and column_name = 'deviceId'
          and data_type = 'integer'
          and is_nullable = 'NO'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'deviceCmds'
          and column_name = 'cmd'
          and data_type = 'smallint'
          and is_nullable = 'NO'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'deviceCmds'
          and column_name = 'status'
          and data_type = 'smallint'
          and is_nullable = 'NO'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'deviceCmds'
          and column_name = 'created_at'
          and data_type = 'timestamp without time zone'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'deviceCmds'
          and column_name = 'sent_at'
          and data_type = 'timestamp without time zone'
    ) then
        raise exception 'legacy public."deviceCmds" column contract drift';
    end if;

    select count(*)
    into v_invalid_count
    from public."deviceCmds" as c
    where c.cmd not between 1 and 4
       or c.status not between 0 and 1;

    if v_invalid_count <> 0 then
        raise exception 'legacy command rows contain unsupported opcode/status values: %',
            v_invalid_count;
    end if;

    select count(*)
    into v_invalid_count
    from public."deviceCmds" as c
    left join public.device as d
      on d."deviceId" = c."deviceId"
    where d."deviceId" is null;

    if v_invalid_count <> 0 then
        raise exception 'legacy command rows contain orphan device references: %',
            v_invalid_count;
    end if;

    select count(*)
    into v_invalid_count
    from (
        select d."deviceIMEI"
        from public.device as d
        where d."deviceIMEI" is not null
        group by d."deviceIMEI"
        having count(*) > 1
    ) as duplicates;

    if v_invalid_count <> 0 then
        raise exception 'deviceIMEI duplicate groups prevent deterministic claim: %',
            v_invalid_count;
    end if;
end;
$precheck$;

rollback;
