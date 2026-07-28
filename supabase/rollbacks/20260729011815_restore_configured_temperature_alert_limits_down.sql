begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

do $rollback$
declare
    v_definition text;
    v_original text :=
        'v_state.high_notification_count < v_max_notifications';
    v_replacement text :=
        'v_state.high_notification_count < 32767';
begin
    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'
            ::pg_catalog.regprocedure
    )
    into v_definition;

    if pg_catalog.strpos(v_definition, v_original) = 0
       or pg_catalog.strpos(v_definition, v_replacement) <> 0 then
        raise exception
            'configured temperature alert trigger definition drift';
    end if;

    v_definition := pg_catalog.replace(
        v_definition,
        v_original,
        v_replacement
    );
    execute v_definition;
end;
$rollback$;

commit;
