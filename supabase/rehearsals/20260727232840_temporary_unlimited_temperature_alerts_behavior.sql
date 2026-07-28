begin;

set local statement_timeout = '30s';

do $behavior$
declare
    v_sensor_pk integer;
    v_before_count smallint;
    v_after_count smallint;
    v_incident_id bigint;
    v_rows integer;
begin
    select s.user_sensor_pk, s.high_notification_count, s.incident_id
    into v_sensor_pk, v_before_count, v_incident_id
    from public.temperature_alert_state as s
    where s.is_high
    order by s.updated_at desc
    limit 1
    for update;

    if not found then
        raise exception 'active high sensor is required';
    end if;

    update public.temperature_alert_state
    set high_notification_count = 3,
        last_high_notification_at =
            pg_catalog.clock_timestamp() - interval '20 minutes'
    where user_sensor_pk = v_sensor_pk;

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
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_pk
      and o.incident_id = v_incident_id
      and o.event_key = 'temperature_high'
      and o.notification_ordinal = 4;

    if v_after_count <> 4 or v_rows <> 1 then
        raise exception 'fourth high notification was not admitted';
    end if;
end;
$behavior$;

rollback;
