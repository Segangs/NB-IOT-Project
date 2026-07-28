begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

do $rollback_guard$
begin
    if exists (
        select 1
        from public.temperature_alert_state
        where high_notification_count > 3
    ) or exists (
        select 1
        from public.temperature_alert_outbox
        where notification_ordinal > 3
    ) then
        raise exception
            'unlimited alert data exists; explicit data decision required';
    end if;
end;
$rollback_guard$;

alter table public.temperature_alert_state
    drop constraint temperature_alert_state_high_notification_count_check,
    drop constraint temperature_alert_state_high_notification_time_check,
    add constraint temperature_alert_state_high_notification_count_check
        check (high_notification_count between 0 and 3),
    add constraint temperature_alert_state_high_notification_time_check
        check (
            (
                is_high
                and high_notification_count between 1 and 3
                and last_high_notification_at is not null
            )
            or
            (
                not is_high
                and high_notification_count between 0 and 3
            )
        );

alter table public.temperature_alert_outbox
    drop constraint temperature_alert_outbox_notification_ordinal_check,
    add constraint temperature_alert_outbox_notification_ordinal_check
        check (notification_ordinal between 1 and 3);

do $rollback$
declare
    v_definition text;
    v_original text :=
        'v_state.high_notification_count < 32767';
    v_replacement text :=
        'v_state.high_notification_count < v_max_notifications';
begin
    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'
            ::pg_catalog.regprocedure
    )
    into v_definition;

    if pg_catalog.strpos(v_definition, v_original) = 0 then
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
$rollback$;

commit;
