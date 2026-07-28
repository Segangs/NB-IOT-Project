begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

do $migration$
declare
    v_definition text;
    v_original text :=
        'v_state.high_notification_count < 32767';
    v_replacement text :=
        'v_state.high_notification_count < v_max_notifications';
begin
    if exists (
        select 1
        from public.temperature_alert_preference
        where max_notifications not between 1 and 3
    ) then
        raise exception 'temperature alert preference is out of range';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'
            ::pg_catalog.regprocedure
    )
    into v_definition;

    if pg_catalog.strpos(v_definition, v_original) = 0
       or pg_catalog.strpos(v_definition, v_replacement) <> 0 then
        raise exception
            'temporary unlimited trigger definition drift';
    end if;

    v_definition := pg_catalog.replace(
        v_definition,
        v_original,
        v_replacement
    );
    execute v_definition;
end;
$migration$;

commit;
