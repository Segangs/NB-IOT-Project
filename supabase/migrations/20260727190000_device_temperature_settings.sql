begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public."USER_SENSOR"
    in share row exclusive mode;
lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

create table public.temperature_alert_preference (
    user_sensor_pk integer primary key
        references public."USER_SENSOR" ("Id") on delete cascade,
    max_notifications smallint not null default 3,
    created_at timestamptz not null default statement_timestamp(),
    updated_at timestamptz not null default statement_timestamp(),
    constraint temperature_alert_preference_max_notifications_check
        check (max_notifications between 1 and 3)
);

alter table public.temperature_alert_preference
    enable row level security;

revoke all on table public.temperature_alert_preference
    from public, anon, authenticated;
grant select, insert, update, delete on table
    public.temperature_alert_preference to service_role;

create function public.get_device_temperature_settings(
    p_user_id integer,
    p_workplace_id integer,
    p_device_id integer
)
returns table (
    user_sensor_pk integer,
    user_sensor_id integer,
    device_id integer,
    sensor_type text,
    upper_limit real,
    max_notifications smallint,
    latest_value real,
    latest_observed_at timestamptz
)
language sql
stable
security definer
set search_path = ''
as $function$
    select
        s."Id" as user_sensor_pk,
        s."userSensorId" as user_sensor_id,
        s."deviceId" as device_id,
        c."sensorCtgyType" as sensor_type,
        case
            when s."setTmpUpLimit" is null
              or s."setTmpUpLimit"::text in (
                  'NaN',
                  'Infinity',
                  '-Infinity'
              )
            then -10.0::real
            else s."setTmpUpLimit"
        end as upper_limit,
        coalesce(p.max_notifications, 3)::smallint
            as max_notifications,
        latest."sensorValue" as latest_value,
        latest."sensorvaluetime" at time zone 'Asia/Seoul'
            as latest_observed_at
    from public."USER_SENSOR" as s
    join public."SENSOR_CTGY" as c
      on c."sensorCtgyId" = s."sensorCtgyId"
    join public.device as d
      on d."deviceId" = s."deviceId"
    join public.userworkplace as w
      on w."userWorkplaceId" = d."userWorkplaceId"
     and w."userId" = d."userId"
    left join public.temperature_alert_preference as p
      on p.user_sensor_pk = s."Id"
    left join lateral (
        select
            sv."sensorValue",
            sv."sensorvaluetime"
        from public.sensorvalue as sv
        where sv."sensorId" = s."Id"
          and sv."sensorValue" is not null
          and sv."sensorValue"::text not in (
              'NaN',
              'Infinity',
              '-Infinity'
          )
          and sv."sensorvaluetime" is not null
        order by sv."sensorValueId" desc
        limit 1
    ) as latest on true
    where p_user_id > 0
      and p_workplace_id > 0
      and p_device_id > 0
      and s."deviceId" = p_device_id
      and c."sensorCtgyType" = 'TMP'
      and d."userId" = p_user_id
      and d."userWorkplaceId" = p_workplace_id
    order by s."userSensorId", s."Id";
$function$;

create function public.update_device_temperature_settings(
    p_user_id integer,
    p_workplace_id integer,
    p_device_id integer,
    p_updates jsonb
)
returns table (
    user_sensor_pk integer,
    user_sensor_id integer,
    device_id integer,
    sensor_type text,
    upper_limit real,
    max_notifications smallint,
    latest_value real,
    latest_observed_at timestamptz
)
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_item jsonb;
    v_sensor_pk integer;
    v_upper_limit numeric;
    v_max_notifications smallint;
    v_seen integer[] := array[]::integer[];
    v_owner_count integer;
begin
    if p_user_id is null or p_user_id < 1
       or p_workplace_id is null or p_workplace_id < 1
       or p_device_id is null or p_device_id < 1 then
        raise exception 'invalid settings owner scope'
            using errcode = '22023';
    end if;
    if p_updates is null
       or pg_catalog.jsonb_typeof(p_updates) <> 'array'
       or pg_catalog.jsonb_array_length(p_updates) not between 1 and 2 then
        raise exception 'settings updates must contain one or two sensors'
            using errcode = '22023';
    end if;

    perform pg_catalog.pg_advisory_xact_lock(
        719027,
        p_device_id
    );

    for v_item in
        select value
        from pg_catalog.jsonb_array_elements(p_updates)
    loop
        if pg_catalog.jsonb_typeof(v_item) <> 'object' then
            raise exception 'sensor update shape is invalid'
                using errcode = '22023';
        end if;

        select pg_catalog.count(*)::integer
        into v_owner_count
        from pg_catalog.jsonb_object_keys(v_item);

        if v_owner_count <> 3
           or not v_item ?& array[
               'sensor_pk',
               'upper_limit',
               'max_notifications'
           ] then
            raise exception 'sensor update shape is invalid'
                using errcode = '22023';
        end if;
        if (v_item ->> 'sensor_pk') !~ '^[1-9][0-9]*$' then
            raise exception 'sensor identifier is invalid'
                using errcode = '22023';
        end if;
        v_sensor_pk := (v_item ->> 'sensor_pk')::integer;
        if v_sensor_pk = any(v_seen) then
            raise exception 'duplicate sensor update'
                using errcode = '22023';
        end if;
        v_seen := pg_catalog.array_append(v_seen, v_sensor_pk);

        if (v_item ->> 'upper_limit')
               !~ '^-?[0-9]+([.][0-9]+)?$' then
            raise exception
                'upper limit must be -50..25 in 0.5 degree steps'
                using errcode = '22023';
        end if;
        v_upper_limit := (v_item ->> 'upper_limit')::numeric;
        if v_upper_limit < -50
           or v_upper_limit > 25
           or mod(v_upper_limit * 2, 1) <> 0 then
            raise exception
                'upper limit must be -50..25 in 0.5 degree steps'
                using errcode = '22023';
        end if;

        if (v_item ->> 'max_notifications') !~ '^[1-3]$' then
            raise exception
                'max notifications must be between 1 and 3'
                using errcode = '22023';
        end if;
        v_max_notifications :=
            (v_item ->> 'max_notifications')::smallint;

        select pg_catalog.count(*)::integer
        into v_owner_count
        from public."USER_SENSOR" as s
        join public."SENSOR_CTGY" as c
          on c."sensorCtgyId" = s."sensorCtgyId"
        join public.device as d
          on d."deviceId" = s."deviceId"
        join public.userworkplace as w
          on w."userWorkplaceId" = d."userWorkplaceId"
         and w."userId" = d."userId"
        where s."Id" = v_sensor_pk
          and s."deviceId" = p_device_id
          and c."sensorCtgyType" = 'TMP'
          and d."userId" = p_user_id
          and d."userWorkplaceId" = p_workplace_id;
        if v_owner_count <> 1 then
            raise exception 'sensor update escaped owner device scope'
                using errcode = '42501';
        end if;
    end loop;

    perform pg_catalog.pg_advisory_xact_lock(
        sensor_id::bigint
    )
    from pg_catalog.unnest(v_seen) as sensor_ids(sensor_id)
    order by sensor_id;

    for v_item in
        select value
        from pg_catalog.jsonb_array_elements(p_updates)
    loop
        v_sensor_pk := (v_item ->> 'sensor_pk')::integer;
        v_upper_limit := (v_item ->> 'upper_limit')::numeric;
        v_max_notifications :=
            (v_item ->> 'max_notifications')::smallint;

        update public."USER_SENSOR" as s
        set "setTmpUpLimit" = v_upper_limit::real
        where s."Id" = v_sensor_pk;

        insert into public.temperature_alert_preference (
            user_sensor_pk,
            max_notifications,
            created_at,
            updated_at
        )
        values (
            v_sensor_pk,
            v_max_notifications,
            pg_catalog.statement_timestamp(),
            pg_catalog.statement_timestamp()
        )
        on conflict on constraint
            temperature_alert_preference_pkey do update
        set max_notifications = excluded.max_notifications,
            updated_at = excluded.updated_at;
    end loop;

    return query
    select *
    from public.get_device_temperature_settings(
        p_user_id,
        p_workplace_id,
        p_device_id
    );
end;
$function$;

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
    v_max_notifications smallint;
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

    select coalesce(p.max_notifications, 3)::smallint
    into v_max_notifications
    from (select 1) as singleton
    left join public.temperature_alert_preference as p
      on p.user_sensor_pk = v_sensor."Id";

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
           and v_state.high_notification_count < v_max_notifications
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

revoke all on function
    public.get_device_temperature_settings(integer, integer, integer)
    from public, anon, authenticated;
grant execute on function
    public.get_device_temperature_settings(integer, integer, integer)
    to service_role;
revoke all on function
    public.update_device_temperature_settings(
        integer,
        integer,
        integer,
        jsonb
    )
    from public, anon, authenticated;
grant execute on function
    public.update_device_temperature_settings(
        integer,
        integer,
        integer,
        jsonb
    )
    to service_role;
revoke all on function public.capture_temperature_alert_transition()
    from public, anon, authenticated;
grant execute on function public.capture_temperature_alert_transition()
    to service_role;

commit;
