begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

alter table public.temperature_alert_state
    drop constraint temperature_alert_state_high_notification_count_check,
    drop constraint temperature_alert_state_high_notification_time_check,
    add constraint temperature_alert_state_high_notification_count_check
        check (high_notification_count between 0 and 32767),
    add constraint temperature_alert_state_high_notification_time_check
        check (
            (
                is_high
                and high_notification_count between 1 and 32767
                and last_high_notification_at is not null
            )
            or
            (
                not is_high
                and high_notification_count between 0 and 32767
            )
        );

alter table public.temperature_alert_outbox
    drop constraint temperature_alert_outbox_notification_ordinal_check,
    add constraint temperature_alert_outbox_notification_ordinal_check
        check (notification_ordinal between 1 and 32767);

do $migration$
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

    if pg_catalog.strpos(v_definition, v_original) = 0 then
        raise exception
            'temperature alert trigger limit expression drift';
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
