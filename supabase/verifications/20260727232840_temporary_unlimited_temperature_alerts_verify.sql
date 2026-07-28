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
           '%v_state.high_notification_count < 32767%'
       or v_definition not ilike '%interval ''20 minutes''%'
       or v_definition ilike
           '%v_state.high_notification_count < v_max_notifications%' then
        raise exception 'temporary unlimited trigger verification failed';
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
        raise exception 'temporary state count constraint drift';
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
        raise exception 'temporary outbox ordinal constraint drift';
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
    user_sensor_pk,
    is_high,
    high_notification_count,
    last_high_notification_at
from public.temperature_alert_state
order by user_sensor_pk;

rollback;
