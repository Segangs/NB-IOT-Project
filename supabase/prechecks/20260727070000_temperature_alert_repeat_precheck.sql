begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_definition text;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if to_regclass('public.temperature_alert_state') is null
       or to_regclass('public.temperature_alert_outbox') is null
       or to_regprocedure(
          'public.capture_temperature_alert_transition()'
       ) is null
       or to_regprocedure(
          'public.drain_temperature_alert_outbox(integer)'
       ) is null then
        raise exception 'base temperature alert contract is missing';
    end if;

    if exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and (
              (
                  table_name = 'temperature_alert_state'
                  and column_name in (
                      'high_notification_count',
                      'last_high_notification_at'
                  )
              )
              or
              (
                  table_name = 'temperature_alert_outbox'
                  and column_name = 'notification_ordinal'
              )
          )
    ) then
        raise exception 'temperature alert repeat column already exists';
    end if;

    select pg_catalog.pg_get_constraintdef(c.oid)
    into v_definition
    from pg_catalog.pg_constraint as c
    where c.conrelid =
              'public.temperature_alert_outbox'::regclass
      and c.conname =
              'temperature_alert_outbox_incident_event_unique'
      and c.contype = 'u';
    if v_definition is null
       or pg_catalog.regexp_replace(
              pg_catalog.lower(v_definition),
              '\s+',
              '',
              'g'
          ) <> 'unique(user_sensor_pk,incident_id,event_key)' then
        raise exception
            'temperature_alert_outbox_incident_event_unique drift';
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
        from public.temperature_alert_state as s
        left join public.temperature_alert_outbox as o
          on o.user_sensor_pk = s.user_sensor_pk
         and o.incident_id = s.incident_id
         and o.event_key = 'temperature_high'
        where s.is_high
        group by s.user_sensor_pk
        having pg_catalog.count(o.temperature_alert_outbox_id) <> 1
    ) then
        raise exception
            'each active high incident requires one base high outbox';
    end if;

    if exists (
        select 1
        from public.temperature_alert_outbox
        group by user_sensor_pk, incident_id, event_key
        having pg_catalog.count(*) > 1
    ) then
        raise exception 'base outbox incident event cardinality drift';
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
        raise exception 'base temperature alert function hardening drift';
    end if;
end;
$precheck$;

select
    (
        select pg_catalog.count(*)
        from public.temperature_alert_state
        where is_high
    ) as active_high_state_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where status = 'processing'
    ) as processing_outbox_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
    ) as outbox_count;

rollback;
