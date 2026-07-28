begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
begin
    perform interval '20 minutes';

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'temperature_alert_state'
          and column_name = 'high_notification_count'
          and data_type = 'smallint'
          and is_nullable = 'NO'
          and column_default = '0'
    ) then
        raise exception 'high_notification_count contract drift';
    end if;

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'temperature_alert_state'
          and column_name = 'last_high_notification_at'
          and data_type = 'timestamp with time zone'
          and is_nullable = 'YES'
    ) then
        raise exception 'last_high_notification_at contract drift';
    end if;

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'temperature_alert_outbox'
          and column_name = 'notification_ordinal'
          and data_type = 'smallint'
          and is_nullable = 'NO'
          and column_default = '1'
    ) then
        raise exception 'notification_ordinal contract drift';
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
          ) <>
          'unique(user_sensor_pk,incident_id,event_key,notification_ordinal)'
    then
        raise exception 'repeat outbox unique contract drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
                  'public.temperature_alert_state'::regclass
          and c.conname =
                  'temperature_alert_state_high_notification_count_check'
          and pg_catalog.pg_get_constraintdef(c.oid)
              ilike
              '%high_notification_count >= 0%high_notification_count <= 3%'
    ) then
        raise exception 'high notification count check missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
                  'public.temperature_alert_outbox'::regclass
          and c.conname =
                  'temperature_alert_outbox_notification_ordinal_check'
          and pg_catalog.pg_get_constraintdef(c.oid)
              ilike
              '%notification_ordinal >= 1%notification_ordinal <= 3%'
    ) then
        raise exception 'notification ordinal check missing';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'::regprocedure
    )
    into v_definition;
    if v_definition not ilike '%interval ''20 minutes''%'
       or v_definition not ilike
              '%v_state.high_notification_count < 3%'
       or v_definition not ilike '%notification_ordinal%' then
        raise exception 'repeat transition function drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.drain_temperature_alert_outbox(integer)'::regprocedure
    )
    into v_definition;
    if v_definition not ilike
           '%v_item.notification_ordinal::text%'
       or v_definition not ilike
           '%public.enqueue_temp_alert_msg_send(%' then
        raise exception 'repeat drain function drift';
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
              or p.proowner <>
                 (
                     select r.oid
                     from pg_catalog.pg_roles as r
                     where r.rolname = 'postgres'
                 )
              or not exists (
                  select 1
                  from pg_catalog.unnest(p.proconfig) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
          )
    ) then
        raise exception 'repeat function security contract drift';
    end if;

    if pg_catalog.has_function_privilege(
           'anon',
           'public.capture_temperature_alert_transition()',
           'EXECUTE'
       )
       or pg_catalog.has_function_privilege(
           'authenticated',
           'public.capture_temperature_alert_transition()',
           'EXECUTE'
       )
       or not pg_catalog.has_function_privilege(
           'service_role',
           'public.capture_temperature_alert_transition()',
           'EXECUTE'
       )
       or pg_catalog.has_function_privilege(
           'anon',
           'public.drain_temperature_alert_outbox(integer)',
           'EXECUTE'
       )
       or pg_catalog.has_function_privilege(
           'authenticated',
           'public.drain_temperature_alert_outbox(integer)',
           'EXECUTE'
       )
       or not pg_catalog.has_function_privilege(
           'service_role',
           'public.drain_temperature_alert_outbox(integer)',
           'EXECUTE'
       ) then
        raise exception 'repeat function ACL drift';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_class as c
        where c.oid in (
            'public.temperature_alert_state'::regclass,
            'public.temperature_alert_outbox'::regclass
        )
          and not c.relrowsecurity
    ) then
        raise exception 'temperature alert relrowsecurity drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_capture_temperature_alert_transition'
          and not t.tgisinternal
          and t.tgenabled = 'O'
          and t.tgfoid =
              'public.capture_temperature_alert_transition()'::regprocedure
    ) then
        raise exception 'temperature alert trigger drift';
    end if;
end;
$verify$;

select
    (
        select pg_catalog.count(*)
        from public.temperature_alert_state
        where is_high
    ) as active_high_state_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where notification_ordinal = 2
    ) as second_notification_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where notification_ordinal = 3
    ) as third_notification_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where status = 'processing'
    ) as processing_outbox_count;

rollback;
