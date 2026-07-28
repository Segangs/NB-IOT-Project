begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
    v_index_definition text;
begin
    if to_regclass('public.device_power_event') is null then
        raise exception 'power event table is missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_class as c
        join pg_catalog.pg_namespace as n on n.oid = c.relnamespace
        where n.nspname = 'public'
          and c.relname = 'device_power_event'
          and c.relrowsecurity
    ) then
        raise exception 'power event RLS boundary drift';
    end if;

    if to_regprocedure(
           'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)'
       ) is null then
        raise exception 'power event RPC is missing';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid =
              'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)'::regprocedure
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
        raise exception 'power event RPC hardening drift';
    end if;

    if not has_function_privilege(
           'anon',
           'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)',
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)',
           'EXECUTE'
       )
       or not has_function_privilege(
           'service_role',
           'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)',
           'EXECUTE'
       ) then
        raise exception 'power event RPC privilege drift';
    end if;

    select pg_catalog.pg_get_indexdef(i.indexrelid)
    into strict v_index_definition
    from pg_catalog.pg_index as i
    join pg_catalog.pg_class as c on c.oid = i.indexrelid
    where c.relname = 'device_power_event_idempotency_unique';

    if position(
           'source_device_id, source_origin, event_type, incident_id, event_sequence'
           in v_index_definition
       ) = 0 then
        raise exception 'device_power_event_idempotency_unique drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.ingest_device_power_event(text,smallint,smallint,bigint,smallint,smallint,integer,integer,bigint,boolean,text)'::regprocedure
    )
    into strict v_definition;

    if position('nb_iot_event_gateway_secret' in v_definition) = 0
       or position('bizp_2026071315030116762724896' in v_definition) = 0
       or position('bizp_2026071315101276625891453' in v_definition) = 0
       or position('bizp_2026071315073916762730996' in v_definition) = 0 then
        raise exception 'power event runtime contract drift';
    end if;

    if not exists (
        select 1
        from vault.decrypted_secrets as s
        join vault.secrets as d on d.id = s.id
        where s.name = 'nb_iot_event_gateway_secret'
          and d.description =
              'NB-IOT power event gateway migration 20260727130000;secret_id='
              || s.id::text
          and nullif(s.decrypted_secret, '') is not null
    ) then
        raise exception 'power event gateway secret provenance drift';
    end if;
end;
$verify$;

select
    pg_catalog.count(*) as power_event_count,
    pg_catalog.count(*) filter (where event_type = 4) as adapter_removed_count,
    pg_catalog.count(*) filter (where event_type = 5) as adapter_restored_count,
    pg_catalog.count(*) filter (where event_type = 6) as power_shutdown_count
from public.device_power_event;

rollback;
