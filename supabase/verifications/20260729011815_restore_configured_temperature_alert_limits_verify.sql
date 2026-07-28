begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
begin
    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'
            ::pg_catalog.regprocedure
    )
    into v_definition;

    if v_definition not ilike
           '%v_state.high_notification_count < v_max_notifications%'
       or v_definition not ilike '%interval ''20 minutes''%'
       or v_definition ilike
           '%v_state.high_notification_count < 32767%' then
        raise exception 'configured temperature alert limit verification failed';
    end if;

    if exists (
        select 1
        from public.temperature_alert_preference
        where max_notifications not between 1 and 3
    ) then
        raise exception 'temperature alert preference is out of range';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
              'public.temperature_alert_state'::pg_catalog.regclass
          and c.conname =
              'temperature_alert_state_high_notification_count_check'
          and pg_catalog.pg_get_constraintdef(c.oid)
              ilike
              '%high_notification_count >= 0%'
              || '%high_notification_count <= 32767%'
    ) then
        raise exception 'preserved state count constraint drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
              'public.temperature_alert_outbox'::pg_catalog.regclass
          and c.conname =
              'temperature_alert_outbox_notification_ordinal_check'
          and pg_catalog.pg_get_constraintdef(c.oid)
              ilike
              '%notification_ordinal >= 1%'
              || '%notification_ordinal <= 32767%'
    ) then
        raise exception 'preserved outbox ordinal constraint drift';
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
       ) then
        raise exception 'temperature alert trigger ACL drift';
    end if;
end;
$verify$;

select
    (
        select pg_catalog.count(*)
        from public.temperature_alert_state
        where high_notification_count > 3
    ) as preserved_over_three_state_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where notification_ordinal > 3
    ) as preserved_over_three_outbox_count;

rollback;
