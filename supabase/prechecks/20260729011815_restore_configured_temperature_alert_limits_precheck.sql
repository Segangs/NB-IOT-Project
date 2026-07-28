begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_definition text;
begin
    if to_regclass('public.temperature_alert_state') is null
       or to_regclass('public.temperature_alert_outbox') is null
       or to_regclass('public.temperature_alert_preference') is null
       or to_regprocedure(
           'public.capture_temperature_alert_transition()'
       ) is null then
        raise exception 'temperature alert contract is missing';
    end if;

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
        raise exception 'temporary unlimited trigger definition drift';
    end if;

    if exists (
        select 1
        from public.temperature_alert_preference
        where max_notifications not between 1 and 3
    ) then
        raise exception 'temperature alert preference is out of range';
    end if;

    if exists (
        select 1
        from public.temperature_alert_outbox
        where status = 'processing'
    ) then
        raise exception 'processing outbox must be zero';
    end if;
end;
$precheck$;

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
