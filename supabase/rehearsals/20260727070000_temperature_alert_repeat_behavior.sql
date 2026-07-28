begin;

set local statement_timeout = '30s';

do $behavior$
declare
    v_sensor_id integer;
    v_high_value real;
    v_low_value real;
    v_original_incident bigint;
    v_first_incident bigint;
    v_next_incident bigint;
    v_count smallint;
    v_rows bigint;
begin
    select
        s.user_sensor_pk,
        s.last_upper_limit + 1,
        s.last_upper_limit - 1,
        s.incident_id
    into
        v_sensor_id,
        v_high_value,
        v_low_value,
        v_original_incident
    from public.temperature_alert_state as s
    where s.is_high
    order by s.user_sensor_pk
    limit 1
    for update;

    if not found then
        raise exception 'behavior rehearsal requires one active high state';
    end if;

    update public.temperature_alert_state
    set is_high = false,
        updated_at = pg_catalog.clock_timestamp()
    where user_sensor_pk = v_sensor_id;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.incident_id, s.high_notification_count
    into v_first_incident, v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    if v_first_incident = v_original_incident or v_count <> 1 then
        raise exception 'fresh high incident did not start at ordinal 1';
    end if;

    select pg_catalog.count(*)
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_id
      and o.incident_id = v_first_incident
      and o.event_key = 'temperature_high'
      and o.notification_ordinal = 1;
    if v_rows <> 1 then
        raise exception 'fresh high ordinal 1 cardinality drift';
    end if;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    if v_count <> 1 then
        raise exception 'sub-20-minute high created a repeat';
    end if;

    update public.temperature_alert_state
    set last_high_notification_at =
            pg_catalog.clock_timestamp() - interval '20 minutes'
    where user_sensor_pk = v_sensor_id;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    select pg_catalog.count(*)
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_id
      and o.incident_id = v_first_incident
      and o.event_key = 'temperature_high'
      and o.notification_ordinal = 2;
    if v_count <> 2 or v_rows <> 1 then
        raise exception 'second high notification contract drift';
    end if;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    if v_count <> 2 then
        raise exception 'immediate repeat after ordinal 2 was accepted';
    end if;

    update public.temperature_alert_state
    set last_high_notification_at =
            pg_catalog.clock_timestamp() - interval '20 minutes'
    where user_sensor_pk = v_sensor_id;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    select pg_catalog.count(*)
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_id
      and o.incident_id = v_first_incident
      and o.event_key = 'temperature_high'
      and o.notification_ordinal = 3;
    if v_count <> 3 or v_rows <> 1 then
        raise exception 'third high notification contract drift';
    end if;

    update public.temperature_alert_state
    set last_high_notification_at =
            pg_catalog.clock_timestamp() - interval '20 minutes'
    where user_sensor_pk = v_sensor_id;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    select pg_catalog.count(*)
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_id
      and o.incident_id = v_first_incident
      and o.event_key = 'temperature_high';
    if v_count <> 3 or v_rows <> 3 then
        raise exception 'high notification count exceeded three';
    end if;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_low_value,
        localtimestamp
    );

    select pg_catalog.count(*)
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_id
      and o.incident_id = v_first_incident
      and o.event_key = 'temperature_recovered'
      and o.notification_ordinal = 1;
    if v_rows <> 1
       or (
           select s.is_high
           from public.temperature_alert_state as s
           where s.user_sensor_pk = v_sensor_id
       ) then
        raise exception 'temperature recovery contract drift';
    end if;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (
        v_sensor_id,
        v_high_value,
        localtimestamp
    );

    select s.incident_id, s.high_notification_count
    into v_next_incident, v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_id;
    if v_next_incident = v_first_incident or v_count <> 1 then
        raise exception 're-high did not start a fresh incident';
    end if;

    select pg_catalog.count(*)
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_id
      and o.incident_id = v_next_incident
      and o.event_key = 'temperature_high'
      and o.notification_ordinal = 1;
    if v_rows <> 1 then
        raise exception 're-high ordinal 1 cardinality drift';
    end if;
end;
$behavior$;

rollback;
