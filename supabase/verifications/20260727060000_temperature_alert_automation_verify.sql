begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_count integer;
begin
    if to_regclass('public.temperature_alert_state') is null
       or to_regclass('public.temperature_alert_outbox') is null
       or to_regclass('public.temperature_alert_incident_id_seq') is null
       or to_regprocedure(
          'public.capture_temperature_alert_transition()'
       ) is null
       or to_regprocedure(
          'public.drain_temperature_alert_outbox(integer)'
       ) is null then
        raise exception 'temperature alert object is missing';
    end if;

    select pg_catalog.count(*)::integer
    into v_count
    from pg_catalog.pg_class as c
    join pg_catalog.pg_namespace as n on n.oid = c.relnamespace
    where n.nspname = 'public'
      and c.relname in (
          'temperature_alert_state',
          'temperature_alert_outbox'
      )
      and c.relrowsecurity;
    if v_count <> 2 then
        raise exception 'temperature alert RLS contract drift';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_class as c
        join pg_catalog.pg_namespace as n on n.oid = c.relnamespace
        where n.nspname = 'public'
          and c.relname in (
              'temperature_alert_state',
              'temperature_alert_outbox',
              'temperature_alert_incident_id_seq'
          )
          and pg_catalog.pg_get_userbyid(c.relowner) <> current_user
    ) or exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.capture_temperature_alert_transition()'::regprocedure,
            'public.drain_temperature_alert_outbox(integer)'::regprocedure
        )
          and pg_catalog.pg_get_userbyid(p.proowner) <> current_user
    ) then
        raise exception 'temperature alert owner drift';
    end if;

    if (
        select pg_catalog.count(*)
        from pg_catalog.pg_policies as p
        where p.schemaname = 'public'
          and p.tablename in (
              'temperature_alert_state',
              'temperature_alert_outbox'
          )
    ) <> 0 then
        raise exception 'temperature alert policy count drift';
    end if;

    if has_table_privilege(
           'anon', 'public.temperature_alert_state', 'SELECT'
       )
       or has_table_privilege(
           'authenticated',
           'public.temperature_alert_state',
           'SELECT'
       )
       or has_table_privilege(
           'anon', 'public.temperature_alert_outbox', 'SELECT'
       )
       or has_table_privilege(
           'authenticated',
           'public.temperature_alert_outbox',
           'SELECT'
       )
       or not has_table_privilege(
           'service_role',
           'public.temperature_alert_state',
           'SELECT,INSERT,UPDATE,DELETE'
       )
       or not has_table_privilege(
           'service_role',
           'public.temperature_alert_outbox',
           'SELECT,INSERT,UPDATE,DELETE'
       ) then
        raise exception 'temperature alert table privilege drift';
    end if;

    if to_regclass(
        'public.temperature_alert_outbox_claim_idx'
    ) is null then
        raise exception 'temperature alert claim index missing';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.capture_temperature_alert_transition()'::regprocedure,
            'public.drain_temperature_alert_outbox(integer)'::regprocedure
        )
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
        raise exception 'temperature alert function hardening drift';
    end if;

    if has_function_privilege(
           'anon',
           'public.drain_temperature_alert_outbox(integer)',
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           'public.drain_temperature_alert_outbox(integer)',
           'EXECUTE'
       )
       or not has_function_privilege(
           'service_role',
           'public.drain_temperature_alert_outbox(integer)',
           'EXECUTE'
       )
       or has_function_privilege(
           'anon',
           'public.capture_temperature_alert_transition()',
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           'public.capture_temperature_alert_transition()',
           'EXECUTE'
       )
       or not has_function_privilege(
           'service_role',
           'public.capture_temperature_alert_transition()',
           'EXECUTE'
       ) then
        raise exception 'temperature alert drain privilege drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_capture_temperature_alert_transition'
          and t.tgenabled = 'O'
          and not t.tgisinternal
          and t.tgfoid =
              'public.capture_temperature_alert_transition()'::regprocedure
    ) then
        raise exception 'temperature alert trigger drift';
    end if;
end;
$verify$;

select
    (select pg_catalog.count(*) from public.temperature_alert_state)
        as state_count,
    (select pg_catalog.count(*) from public.temperature_alert_outbox)
        as outbox_count;

rollback;
