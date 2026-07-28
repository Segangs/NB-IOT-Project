begin;

set local statement_timeout = '30s';

do $behavior$
declare
    v_user_id integer;
    v_workplace_id integer;
    v_device_id integer;
    v_sensor_one integer;
    v_sensor_two integer;
    v_max smallint;
    v_upper real;
    v_before_upper real;
    v_before_max smallint;
    v_rows integer;
    v_count smallint;
    v_incident bigint;
begin
    select
        d."userId",
        d."userWorkplaceId",
        d."deviceId",
        pg_catalog.min(s."Id"),
        pg_catalog.max(s."Id")
    into
        v_user_id,
        v_workplace_id,
        v_device_id,
        v_sensor_one,
        v_sensor_two
    from public.device as d
    join public.userworkplace as w
      on w."userWorkplaceId" = d."userWorkplaceId"
     and w."userId" = d."userId"
    join public."USER_SENSOR" as s
      on s."deviceId" = d."deviceId"
    join public."SENSOR_CTGY" as c
      on c."sensorCtgyId" = s."sensorCtgyId"
    where c."sensorCtgyType" = 'TMP'
    group by d."userId", d."userWorkplaceId", d."deviceId"
    having pg_catalog.count(*) >= 2
    order by d."deviceId"
    limit 1;

    if not found then
        raise exception
            'behavior requires one owned device with two TEMP sensors';
    end if;

    delete from public.temperature_alert_preference
    where user_sensor_pk in (v_sensor_one, v_sensor_two);

    select s.max_notifications
    into v_max
    from public.get_device_temperature_settings(
        v_user_id,
        v_workplace_id,
        v_device_id
    ) as s
    where s.user_sensor_pk = v_sensor_one;
    if v_max <> 3 then
        raise exception 'default max notifications must be three';
    end if;

    perform public.update_device_temperature_settings(
        v_user_id,
        v_workplace_id,
        v_device_id,
        pg_catalog.jsonb_build_array(
            pg_catalog.jsonb_build_object(
                'sensor_pk', v_sensor_one,
                'upper_limit', '-7.0',
                'max_notifications', 1
            )
        )
    );
    select
        s."setTmpUpLimit",
        p.max_notifications
    into v_upper, v_max
    from public."USER_SENSOR" as s
    join public.temperature_alert_preference as p
      on p.user_sensor_pk = s."Id"
    where s."Id" = v_sensor_one;
    if v_upper <> -7.0 or v_max <> 1 then
        raise exception 'one-sensor update failed';
    end if;

    perform public.update_device_temperature_settings(
        v_user_id,
        v_workplace_id,
        v_device_id,
        pg_catalog.jsonb_build_array(
            pg_catalog.jsonb_build_object(
                'sensor_pk', v_sensor_one,
                'upper_limit', '-8.0',
                'max_notifications', 2
            ),
            pg_catalog.jsonb_build_object(
                'sensor_pk', v_sensor_two,
                'upper_limit', '-10.0',
                'max_notifications', 3
            )
        )
    );
    select pg_catalog.count(*)::integer
    into v_rows
    from public."USER_SENSOR" as s
    join public.temperature_alert_preference as p
      on p.user_sensor_pk = s."Id"
    where (
            s."Id" = v_sensor_one
            and s."setTmpUpLimit" = -8.0
            and p.max_notifications = 2
          )
       or (
            s."Id" = v_sensor_two
            and s."setTmpUpLimit" = -10.0
            and p.max_notifications = 3
          );
    if v_rows <> 2 then
        raise exception 'two-sensor update failed';
    end if;

    select
        s."setTmpUpLimit",
        p.max_notifications
    into v_before_upper, v_before_max
    from public."USER_SENSOR" as s
    join public.temperature_alert_preference as p
      on p.user_sensor_pk = s."Id"
    where s."Id" = v_sensor_one;

    begin
        perform public.update_device_temperature_settings(
            v_user_id,
            v_workplace_id,
            v_device_id,
            pg_catalog.jsonb_build_array(
                pg_catalog.jsonb_build_object(
                    'sensor_pk', v_sensor_one,
                    'upper_limit', '-6.0',
                    'max_notifications', 1
                ),
                pg_catalog.jsonb_build_object(
                    'sensor_pk', 2147483647,
                    'upper_limit', '-9.0',
                    'max_notifications', 2
                )
            )
        );
        raise exception 'invalid second row was accepted';
    exception
        when sqlstate '42501' then
            null;
    end;

    select
        s."setTmpUpLimit",
        p.max_notifications
    into v_upper, v_max
    from public."USER_SENSOR" as s
    join public.temperature_alert_preference as p
      on p.user_sensor_pk = s."Id"
    where s."Id" = v_sensor_one;
    if v_upper <> v_before_upper or v_max <> v_before_max then
        raise exception
            'invalid second row did not roll back first row';
    end if;

    delete from public.temperature_alert_outbox
    where user_sensor_pk = v_sensor_one;
    delete from public.temperature_alert_state
    where user_sensor_pk = v_sensor_one;

    perform public.update_device_temperature_settings(
        v_user_id,
        v_workplace_id,
        v_device_id,
        pg_catalog.jsonb_build_array(
            pg_catalog.jsonb_build_object(
                'sensor_pk', v_sensor_one,
                'upper_limit', '-7.0',
                'max_notifications', 1
            )
        )
    );

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (v_sensor_one, -6.0, localtimestamp);

    select s.incident_id, s.high_notification_count
    into v_incident, v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_one;
    if v_count <> 1 then
        raise exception 'max one incident did not start at one';
    end if;

    update public.temperature_alert_state
    set last_high_notification_at =
            pg_catalog.clock_timestamp() - interval '20 minutes'
    where user_sensor_pk = v_sensor_one;

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (v_sensor_one, -6.0, localtimestamp);

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_one;
    select pg_catalog.count(*)::integer
    into v_rows
    from public.temperature_alert_outbox as o
    where o.user_sensor_pk = v_sensor_one
      and o.incident_id = v_incident
      and o.event_key = 'temperature_high';
    if v_count <> 1 or v_rows <> 1 then
        raise exception 'max one allowed an extra repeat';
    end if;

    perform public.update_device_temperature_settings(
        v_user_id,
        v_workplace_id,
        v_device_id,
        pg_catalog.jsonb_build_array(
            pg_catalog.jsonb_build_object(
                'sensor_pk', v_sensor_one,
                'upper_limit', '-7.0',
                'max_notifications', 3
            )
        )
    );

    insert into public.sensorvalue (
        "sensorId",
        "sensorValue",
        "sensorvaluetime"
    )
    values (v_sensor_one, -6.0, localtimestamp);

    select s.high_notification_count
    into v_count
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor_one;
    if v_count <> 2 then
        raise exception 'increased max did not allow the next repeat';
    end if;
end;
$behavior$;

rollback;
