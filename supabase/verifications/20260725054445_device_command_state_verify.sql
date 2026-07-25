begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_legacy_count bigint;
    v_state_count bigint;
    v_invalid_count bigint;
    v_assign_md5 text;
begin
    if to_regclass('public.device_command_state') is null then
        raise exception 'public.device_command_state is missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_class as c
        where c.oid = 'public.device_command_state'::regclass
          and c.relrowsecurity
          and not c.relforcerowsecurity
    ) then
        raise exception 'device_command_state RLS baseline is not enabled';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_policy as p
        where p.polrelid = 'public.device_command_state'::regclass
    ) then
        raise exception 'unexpected device_command_state policy exists';
    end if;

    if has_table_privilege('anon', 'public.device_command_state', 'SELECT')
       or has_table_privilege('anon', 'public.device_command_state', 'INSERT')
       or has_table_privilege('anon', 'public.device_command_state', 'UPDATE')
       or has_table_privilege('anon', 'public.device_command_state', 'DELETE')
       or has_table_privilege('authenticated', 'public.device_command_state', 'SELECT')
       or has_table_privilege('authenticated', 'public.device_command_state', 'INSERT')
       or has_table_privilege('authenticated', 'public.device_command_state', 'UPDATE')
       or has_table_privilege('authenticated', 'public.device_command_state', 'DELETE') then
        raise exception 'unexpected anon/authenticated table privilege';
    end if;

    if not has_table_privilege('service_role', 'public.device_command_state', 'SELECT')
       or not has_table_privilege('service_role', 'public.device_command_state', 'INSERT')
       or not has_table_privilege('service_role', 'public.device_command_state', 'UPDATE')
       or not has_table_privilege('service_role', 'public.device_command_state', 'DELETE') then
        raise exception 'service_role lacks companion table privilege';
    end if;

    select count(*) into v_legacy_count from public."deviceCmds";
    select count(*) into v_state_count from public.device_command_state;

    if v_state_count <> v_legacy_count then
        raise exception 'backfill count mismatch: legacy %, companion %',
            v_legacy_count, v_state_count;
    end if;

    select count(*)
    into v_invalid_count
    from public.device_command_state as s
    left join public."deviceCmds" as c
      on c."cmdId" = s.cmd_id
    left join public.device as d
      on d."deviceId" = s.device_id
    where c."cmdId" is null
       or d."deviceId" is null
       or c."deviceId" <> s.device_id
       or c.cmd <> s.opcode;

    if v_invalid_count <> 0 then
        raise exception 'companion identity/backfill mismatches: %', v_invalid_count;
    end if;

    select count(*)
    into v_invalid_count
    from public.device_command_state as s
    join public."deviceCmds" as c
      on c."cmdId" = s.cmd_id
    where s.job_id is distinct from c."cmdId"::bigint
       or s.ttl_seconds <> 86400
       or s.legacy_status is distinct from c.status
       or s.expires_at <> s.created_at + interval '1 day'
       or (
           c.created_at is not null
           and s.created_at is distinct from
               c.created_at at time zone 'Asia/Seoul'
       )
       or s.state is distinct from case
           when s.expires_at <= s.updated_at then 'expired'
           when c.status = 0 then 'queued'
           else 'delivered'
       end
       or s.delivery_attempts is distinct from
           case when c.status = 1 then 1 else 0 end
       or (
           c.status = 0
           and (
               s.last_delivered_at is not null
               or s.lease_expires_at is not null
           )
       )
       or (
           c.status = 1
           and (
               s.last_delivered_at is null
               or s.lease_expires_at is distinct from
                   s.last_delivered_at + interval '60 seconds'
           )
       )
       or (
           s.state = 'expired'
           and (
               s.final_at is distinct from s.expires_at
               or s.final_result is distinct from 4::smallint
               or s.final_error is distinct from 5::smallint
           )
       )
       or (
           s.state <> 'expired'
           and (
               s.final_at is not null
               or s.final_result is not null
               or s.final_error is not null
           )
       );

    if v_invalid_count <> 0 then
        raise exception 'backfill state/timestamp mapping mismatch: %',
            v_invalid_count;
    end if;

    if to_regclass('public.idx_device_command_state_claim') is null
       or to_regclass('public.uq_device_command_state_active_request') is null then
        raise exception 'one or more command state indexes are missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid = 'public.device_command_state'::regclass
          and c.conname = 'device_command_state_terminal_check'
          and c.convalidated
    ) then
        raise exception 'device_command_state_terminal_check is missing or invalid';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public."deviceCmds"'::regclass
          and t.tgname = 'trg_sync_device_command_state'
          and not t.tgisinternal
          and t.tgenabled = 'O'
          and t.tgfoid = 'public.sync_device_command_state()'::regprocedure
    ) then
        raise exception 'trg_sync_device_command_state definition mismatch';
    end if;

    if to_regprocedure('public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)') is null
       or to_regprocedure('public.sync_device_command_state()') is null
       or to_regprocedure('public.claim_device_command(text,bigint,bigint,integer)') is null
       or to_regprocedure('public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)') is null then
        raise exception 'one or more command functions are missing';
    end if;

    begin
        perform public.claim_device_command(
            '__P00_VERIFY_NO_DEVICE__',
            1,
            0,
            60
        );
        raise exception 'legacy claim guard did not reject';
    exception
        when object_not_in_prerequisite_state then
            null;
    end;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)'::regprocedure,
            'public.sync_device_command_state()'::regprocedure,
            'public.claim_device_command(text,bigint,bigint,integer)'::regprocedure,
            'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'::regprocedure
        )
          and not exists (
              select 1
              from unnest(coalesce(p.proconfig, array[]::text[])) as cfg
              where cfg ~ '^search_path=(|\"\")$'
          )
    ) then
        raise exception 'one or more command functions lack empty search_path';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.sync_device_command_state()'::regprocedure,
            'public.claim_device_command(text,bigint,bigint,integer)'::regprocedure,
            'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'::regprocedure
        )
          and not p.prosecdef
    ) or exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid =
            'public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)'::regprocedure
          and p.prosecdef
    ) then
        raise exception 'command function SECURITY DEFINER/INVOKER mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)'::regprocedure,
            'public.claim_device_command(text,bigint,bigint,integer)'::regprocedure,
            'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'::regprocedure
        )
          and (
              has_function_privilege('anon', p.oid, 'EXECUTE')
              or has_function_privilege('authenticated', p.oid, 'EXECUTE')
          )
    ) then
        raise exception 'unexpected public/anon/authenticated function execute privilege';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.claim_device_command(text,bigint,bigint,integer)'::regprocedure,
            'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'::regprocedure
        )
          and not has_function_privilege('service_role', p.oid, 'EXECUTE')
    ) then
        raise exception 'service_role lacks command RPC execute privilege';
    end if;

    select md5(pg_get_functiondef('public.assign_device_command()'::regprocedure))
    into v_assign_md5;

    if v_assign_md5 <> 'de97b37ad246c5e509ae06f821a10338' then
        raise exception 'legacy assign_device_command() changed unexpectedly';
    end if;
end;
$verify$;

select
    (select count(*) from public."deviceCmds") as legacy_command_count,
    (select count(*) from public.device_command_state) as companion_command_count,
    (
        select count(*)
        from public.device_command_state
        where state in ('queued', 'delivered')
    ) as claimable_or_leased_count,
    (
        select count(*)
        from public.device_command_state
        where state in ('executed', 'failed', 'expired')
    ) as terminal_count;

rollback;
