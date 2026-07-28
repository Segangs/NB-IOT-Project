begin;

set local statement_timeout = '30s';

do $behavior$
declare
    v_sensor_pk integer;
    v_incident_id bigint;
    v_before_count smallint;
    v_after_count smallint;
    v_before_outbox integer;
    v_after_outbox integer;
begin
    select
        s.user_sensor_pk,
        s.incident_id,
        s.high_notification_count
    into
        v_sensor_pk,
        v_incident_id,
        v_before_count
    from public.temperature_alert_state as s
    where s.is_high
      and s.high_notification_count > 3
    order by s.updated_at desc
    limit 1
    for update;

    if not found then
        raise exception 'preserved fourth notification is required';
    end if;

    select pg_catalog.count(*)::integer
    into v_before_outbox
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_pk
      and o.incident_id = v_incident_id
      and o.event_key = 'temperature_high';

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    select
        s."Id",
        s."setTmpUpLimit" + 1,
        localtimestamp
    from public."USER_SENSOR" as s
    where s."Id" = v_sensor_pk;

    select s.high_notification_count
    into v_after_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_pk;

    select pg_catalog.count(*)::integer
    into v_after_outbox
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_pk
      and o.incident_id = v_incident_id
      and o.event_key = 'temperature_high';

    if v_after_count <> v_before_count
       or v_after_outbox <> v_before_outbox then
        raise exception
            'configured temperature alert limit admitted another notification';
    end if;
end;
$behavior$;

rollback;
