begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.temperature_alert_state
    in share row exclusive mode;
lock table public.temperature_alert_outbox
    in share row exclusive mode;

alter table public.temperature_alert_state
    add column high_notification_count smallint,
    add column last_high_notification_at timestamptz;

alter table public.temperature_alert_outbox
    add column notification_ordinal smallint not null default 1;

with high_rollup as (
    select
        o.user_sensor_pk,
        o.incident_id,
        least(
            pg_catalog.count(*) filter (
                where o.event_key = 'temperature_high'
            ),
            3
        )::smallint as high_count,
        pg_catalog.max(o.created_at) filter (
            where o.event_key = 'temperature_high'
        ) as last_high_at
    from public.temperature_alert_outbox as o
    group by o.user_sensor_pk, o.incident_id
)
update public.temperature_alert_state as s
set high_notification_count =
        coalesce(r.high_count, 0),
    last_high_notification_at = r.last_high_at
from high_rollup as r
where s.user_sensor_pk = r.user_sensor_pk
  and s.incident_id = r.incident_id;

update public.temperature_alert_state
set high_notification_count = 0
where high_notification_count is null;

alter table public.temperature_alert_state
    alter column high_notification_count set default 0,
    alter column high_notification_count set not null,
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
    add constraint temperature_alert_outbox_notification_ordinal_check
        check (notification_ordinal between 1 and 3),
    add constraint temperature_alert_outbox_notification_event_check
        check (
            event_key = 'temperature_high'
            or (
                event_key = 'temperature_recovered'
                and notification_ordinal = 1
            )
        ),
    drop constraint temperature_alert_outbox_incident_event_unique,
    add constraint temperature_alert_outbox_incident_event_unique
        unique (
            user_sensor_pk,
            incident_id,
            event_key,
            notification_ordinal
        );

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

create or replace function public.drain_temperature_alert_outbox(
    p_limit integer default 16
)
returns table (
    temperature_alert_outbox_id bigint,
    msg_send_id bigint,
    result text
)
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_item record;
    v_contact_id bigint;
    v_contact_count integer;
    v_policy_id bigint;
    v_policy_count integer;
    v_workplace_name text;
    v_machine_name text;
    v_machine_count integer;
    v_msg_send_id bigint;
    v_error_code text;
    v_now timestamptz;
    v_template_params jsonb;
begin
    if p_limit is null or p_limit not between 1 and 32 then
        raise exception 'invalid temperature outbox drain limit'
            using errcode = '22023';
    end if;

    for v_item in
        select o.*
        from public.temperature_alert_outbox as o
        where (
                o.status in ('pending', 'retry_wait')
                and o.available_at <= pg_catalog.clock_timestamp()
              )
           or (
                o.status = 'processing'
                and o.lease_expires_at <= pg_catalog.clock_timestamp()
              )
        order by o.available_at, o.temperature_alert_outbox_id
        limit p_limit
        for update skip locked
    loop
        v_now := pg_catalog.clock_timestamp();
        v_error_code := null;
        v_msg_send_id := null;

        update public.temperature_alert_outbox as o
        set status = 'processing',
            attempt_count = o.attempt_count + 1,
            lease_token = pg_catalog.gen_random_uuid(),
            lease_expires_at = v_now + interval '60 seconds',
            last_error_code = null,
            updated_at = v_now
        where o.temperature_alert_outbox_id =
              v_item.temperature_alert_outbox_id
        returning o.attempt_count into v_item.attempt_count;

        select pg_catalog.count(*)::integer, min(c.alert_contact_id)
        into v_contact_count, v_contact_id
        from public.alert_contact as c
        where c.user_id = v_item.source_user_id
          and c.is_active
          and c.consent_status = 'granted'
          and c.consent_revoked_at is null;
        if v_contact_count <> 1 then
            v_error_code := 'contact_cardinality';
        end if;

        select pg_catalog.count(*)::integer, min(p.message_policy_id)
        into v_policy_count, v_policy_id
        from public.message_policy as p
        where p.is_active
          and p.primary_channel = 'alimtalk'
          and not p.allow_sms_fallback
          and p.effective_from <= v_now
          and (
              p.effective_until is null
              or p.effective_until > v_now
          );
        if v_error_code is null and v_policy_count <> 1 then
            v_error_code := 'policy_cardinality';
        end if;

        select w."WorkplaceName"
        into v_workplace_name
        from public.device as d
        join public.userworkplace as w
          on w."userWorkplaceId" = d."userWorkplaceId"
         and w."userId" = d."userId"
        where d."deviceId" = v_item.source_device_id
          and d."userId" = v_item.source_user_id;

        select pg_catalog.count(*)::integer, min(m."userMachineName")
        into v_machine_count, v_machine_name
        from public.usermachine as m
        where m."deviceId" = v_item.source_device_id;

        if v_error_code is null
           and (
               v_workplace_name is null
               or pg_catalog.btrim(v_workplace_name) = ''
               or v_machine_count <> 1
               or v_machine_name is null
               or pg_catalog.btrim(v_machine_name) = ''
           ) then
            v_error_code := 'ownership_missing';
        end if;

        if v_error_code is null then
            if v_item.event_key = 'temperature_high' then
                v_template_params := pg_catalog.jsonb_build_object(
                    'sensorValue', v_item.sensor_value,
                    'WorkplaceName', v_workplace_name,
                    'userMachineName', v_machine_name,
                    'setTmpUpLimit', v_item.upper_limit,
                    'sensorvaluetime',
                    pg_catalog.to_char(
                        v_item.observed_at,
                        'YYYY-MM-DD HH24:MI:SS'
                    )
                );
            else
                v_template_params := pg_catalog.jsonb_build_object(
                    'sensorValue', v_item.sensor_value,
                    'workplaceName', v_workplace_name,
                    'userMachineName', v_machine_name,
                    'setTmpUpLimit', v_item.upper_limit
                );
            end if;

            begin
                v_msg_send_id := public.enqueue_temp_alert_msg_send(
                    v_contact_id,
                    v_policy_id,
                    v_item.source_user_id,
                    v_item.source_device_id,
                    'sensorvalue',
                    v_item.sensor_value_id::bigint,
                    v_item.incident_id,
                    v_item.event_key,
                    v_template_params,
                    'temperature:'
                        || v_item.source_device_id::text
                        || ':' || v_item.user_sensor_pk::text
                        || ':' || v_item.incident_id::text
                        || ':' || v_item.event_key
                        || ':' || v_item.notification_ordinal::text,
                    v_now + interval '24 hours',
                    50::smallint
                );
            exception when others then
                v_error_code := 'enqueue_failed';
            end;
        end if;

        if v_error_code is null then
            update public.temperature_alert_outbox as o
            set status = 'enqueued',
                msg_send_id = v_msg_send_id,
                lease_token = null,
                lease_expires_at = null,
                last_error_code = null,
                updated_at = pg_catalog.clock_timestamp()
            where o.temperature_alert_outbox_id =
                  v_item.temperature_alert_outbox_id;
            temperature_alert_outbox_id :=
                v_item.temperature_alert_outbox_id;
            msg_send_id := v_msg_send_id;
            result := 'enqueued';
            return next;
        else
            update public.temperature_alert_outbox as o
            set status = case
                    when o.attempt_count >= 5 then 'failed'
                    else 'retry_wait'
                end,
                available_at = case
                    when o.attempt_count >= 5 then o.available_at
                    else pg_catalog.clock_timestamp()
                        + least(
                            interval '1 hour',
                            interval '30 seconds'
                                * power(
                                    2::double precision,
                                    o.attempt_count::double precision
                                )
                        )
                end,
                lease_token = null,
                lease_expires_at = null,
                msg_send_id = null,
                last_error_code = v_error_code,
                updated_at = pg_catalog.clock_timestamp()
            where o.temperature_alert_outbox_id =
                  v_item.temperature_alert_outbox_id;
            temperature_alert_outbox_id :=
                v_item.temperature_alert_outbox_id;
            msg_send_id := null;
            result := v_error_code;
            return next;
        end if;
    end loop;
end;
$function$;

revoke all on function public.capture_temperature_alert_transition()
    from public, anon, authenticated;
grant execute on function public.capture_temperature_alert_transition()
    to service_role;
revoke all on function
    public.drain_temperature_alert_outbox(integer)
    from public, anon, authenticated;
grant execute on function
    public.drain_temperature_alert_outbox(integer)
    to service_role;

commit;
