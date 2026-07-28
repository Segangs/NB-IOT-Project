begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public."USER_SENSOR"
    in share row exclusive mode;
lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

create or replace function public.capture_temperature_alert_transition()
returns trigger
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_sensor public."USER_SENSOR"%rowtype;
    v_device_user_id integer;
    v_state public.temperature_alert_state%rowtype;
    v_now timestamptz;
    v_is_high boolean;
    v_incident_id bigint;
    v_notification_ordinal smallint;
begin
    if new."sensorValue" is null
       or new."sensorValue"::text in (
           'NaN',
           'Infinity',
           '-Infinity'
       )
       or new."sensorvaluetime" is null then
        return new;
    end if;

    select s.*
    into v_sensor
    from public."USER_SENSOR" as s
    join public."SENSOR_CTGY" as c
      on c."sensorCtgyId" = s."sensorCtgyId"
    where s."Id" = new."sensorId"
      and c."sensorCtgyType" = 'TMP'
      and s."setTmpUpLimit" is not null
      and s."setTmpUpLimit"::text not in (
          'NaN',
          'Infinity',
          '-Infinity'
      );

    if not found then
        return new;
    end if;

    select d."userId"
    into v_device_user_id
    from public.device as d
    where d."deviceId" = v_sensor."deviceId";
    if not found or v_device_user_id is null then
        return new;
    end if;

    v_now := pg_catalog.clock_timestamp();
    v_is_high := new."sensorValue" > v_sensor."setTmpUpLimit";

    perform pg_catalog.pg_advisory_xact_lock(
        v_sensor."Id"::bigint
    );

    select s.*
    into v_state
    from public.temperature_alert_state as s
    where s.user_sensor_pk = v_sensor."Id"
    for update;

    if not found then
        if v_is_high then
            v_incident_id :=
                nextval('public.temperature_alert_incident_id_seq');
        end if;
        insert into public.temperature_alert_state (
            user_sensor_pk,
            device_id,
            is_high,
            incident_id,
            last_sensor_value_id,
            last_value,
            last_upper_limit,
            high_notification_count,
            last_high_notification_at,
            state_changed_at,
            updated_at
        )
        values (
            v_sensor."Id",
            v_sensor."deviceId",
            v_is_high,
            v_incident_id,
            new."sensorValueId",
            new."sensorValue",
            v_sensor."setTmpUpLimit",
            case when v_is_high then 1 else 0 end,
            case when v_is_high then v_now else null end,
            v_now,
            v_now
        );

        if v_is_high then
            insert into public.temperature_alert_outbox (
                user_sensor_pk,
                sensor_value_id,
                source_user_id,
                source_device_id,
                incident_id,
                event_key,
                notification_ordinal,
                sensor_value,
                upper_limit,
                observed_at
            )
            values (
                v_sensor."Id",
                new."sensorValueId",
                v_device_user_id,
                v_sensor."deviceId",
                v_incident_id,
                'temperature_high',
                1,
                new."sensorValue",
                v_sensor."setTmpUpLimit",
                new."sensorvaluetime"
            )
            on conflict do nothing;
        end if;
        return new;
    end if;

    if v_state.is_high = v_is_high then
        if v_is_high
           and v_state.high_notification_count < 3
           and v_state.last_high_notification_at is not null
           and v_now >=
               v_state.last_high_notification_at
                   + interval '20 minutes' then
            v_notification_ordinal :=
                v_state.high_notification_count + 1;
            update public.temperature_alert_state
            set last_sensor_value_id = new."sensorValueId",
                last_value = new."sensorValue",
                last_upper_limit = v_sensor."setTmpUpLimit",
                high_notification_count = v_notification_ordinal,
                last_high_notification_at = v_now,
                updated_at = v_now
            where user_sensor_pk = v_sensor."Id";
            insert into public.temperature_alert_outbox (
                user_sensor_pk,
                sensor_value_id,
                source_user_id,
                source_device_id,
                incident_id,
                event_key,
                notification_ordinal,
                sensor_value,
                upper_limit,
                observed_at
            )
            values (
                v_sensor."Id",
                new."sensorValueId",
                v_device_user_id,
                v_sensor."deviceId",
                v_state.incident_id,
                'temperature_high',
                v_notification_ordinal,
                new."sensorValue",
                v_sensor."setTmpUpLimit",
                new."sensorvaluetime"
            )
            on conflict do nothing;
        else
            update public.temperature_alert_state
            set last_sensor_value_id = new."sensorValueId",
                last_value = new."sensorValue",
                last_upper_limit = v_sensor."setTmpUpLimit",
                updated_at = v_now
            where user_sensor_pk = v_sensor."Id";
        end if;
        return new;
    end if;

    if v_is_high then
        v_incident_id :=
            nextval('public.temperature_alert_incident_id_seq');
        update public.temperature_alert_state
        set device_id = v_sensor."deviceId",
            is_high = true,
            incident_id = v_incident_id,
            last_sensor_value_id = new."sensorValueId",
            last_value = new."sensorValue",
            last_upper_limit = v_sensor."setTmpUpLimit",
            high_notification_count = 1,
            last_high_notification_at = v_now,
            state_changed_at = v_now,
            updated_at = v_now
        where user_sensor_pk = v_sensor."Id";
        insert into public.temperature_alert_outbox (
            user_sensor_pk,
            sensor_value_id,
            source_user_id,
            source_device_id,
            incident_id,
            event_key,
            notification_ordinal,
            sensor_value,
            upper_limit,
            observed_at
        )
        values (
            v_sensor."Id",
            new."sensorValueId",
            v_device_user_id,
            v_sensor."deviceId",
            v_incident_id,
            'temperature_high',
            1,
            new."sensorValue",
            v_sensor."setTmpUpLimit",
            new."sensorvaluetime"
        )
        on conflict do nothing;
    else
        v_incident_id := v_state.incident_id;
        update public.temperature_alert_state
        set device_id = v_sensor."deviceId",
            is_high = false,
            last_sensor_value_id = new."sensorValueId",
            last_value = new."sensorValue",
            last_upper_limit = v_sensor."setTmpUpLimit",
            state_changed_at = v_now,
            updated_at = v_now
        where user_sensor_pk = v_sensor."Id";
        insert into public.temperature_alert_outbox (
            user_sensor_pk,
            sensor_value_id,
            source_user_id,
            source_device_id,
            incident_id,
            event_key,
            notification_ordinal,
            sensor_value,
            upper_limit,
            observed_at
        )
        values (
            v_sensor."Id",
            new."sensorValueId",
            v_device_user_id,
            v_sensor."deviceId",
            v_incident_id,
            'temperature_recovered',
            1,
            new."sensorValue",
            v_sensor."setTmpUpLimit",
            new."sensorvaluetime"
        )
        on conflict do nothing;
    end if;

    return new;
end;
$function$;

revoke all on function public.capture_temperature_alert_transition()
    from public, anon, authenticated;
grant execute on function public.capture_temperature_alert_transition()
    to service_role;

drop function public.update_device_temperature_settings(
    integer,
    integer,
    integer,
    jsonb
);
drop function public.get_device_temperature_settings(
    integer,
    integer,
    integer
);
drop table public.temperature_alert_preference;

commit;
