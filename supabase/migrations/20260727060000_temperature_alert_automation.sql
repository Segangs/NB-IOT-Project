begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

create sequence public.temperature_alert_incident_id_seq
    as bigint
    minvalue 1
    start with 1
    increment by 1
    no cycle;

create table public.temperature_alert_state (
    user_sensor_pk integer primary key
        references public."USER_SENSOR" ("Id") on delete cascade,
    device_id integer not null,
    is_high boolean not null,
    incident_id bigint,
    last_sensor_value_id integer not null,
    last_value real not null,
    last_upper_limit real not null,
    state_changed_at timestamptz not null,
    updated_at timestamptz not null,
    constraint temperature_alert_state_incident_check
        check (
            (is_high and incident_id is not null and incident_id > 0)
            or (not is_high)
        )
);

create table public.temperature_alert_outbox (
    temperature_alert_outbox_id bigint
        generated always as identity primary key,
    user_sensor_pk integer not null
        references public."USER_SENSOR" ("Id") on delete restrict,
    sensor_value_id integer not null,
    source_user_id integer not null,
    source_device_id integer not null,
    incident_id bigint not null,
    event_key text not null,
    sensor_value real not null,
    upper_limit real not null,
    observed_at timestamp without time zone not null,
    status text not null default 'pending',
    attempt_count integer not null default 0,
    available_at timestamptz not null default statement_timestamp(),
    lease_token uuid,
    lease_expires_at timestamptz,
    msg_send_id bigint,
    last_error_code text,
    created_at timestamptz not null default statement_timestamp(),
    updated_at timestamptz not null default statement_timestamp(),
    constraint temperature_alert_outbox_event_check
        check (
            event_key in (
                'temperature_high',
                'temperature_recovered'
            )
        ),
    constraint temperature_alert_outbox_status_check
        check (
            status in (
                'pending',
                'processing',
                'enqueued',
                'retry_wait',
                'failed'
            )
        ),
    constraint temperature_alert_outbox_attempt_check
        check (attempt_count between 0 and 5),
    constraint temperature_alert_outbox_incident_check
        check (incident_id > 0),
    constraint temperature_alert_outbox_lease_check
        check (
            (
                status = 'processing'
                and lease_token is not null
                and lease_expires_at is not null
            )
            or
            (
                status <> 'processing'
                and lease_token is null
                and lease_expires_at is null
            )
        ),
    constraint temperature_alert_outbox_result_check
        check (
            (status = 'enqueued' and msg_send_id is not null)
            or (status <> 'enqueued' and msg_send_id is null)
        ),
    constraint temperature_alert_outbox_error_check
        check (
            last_error_code is null
            or last_error_code in (
                'contact_cardinality',
                'policy_cardinality',
                'ownership_missing',
                'enqueue_failed'
            )
        ),
    constraint temperature_alert_outbox_sensor_event_unique
        unique (sensor_value_id, event_key),
    constraint temperature_alert_outbox_incident_event_unique
        unique (user_sensor_pk, incident_id, event_key)
);

create index temperature_alert_outbox_claim_idx
    on public.temperature_alert_outbox (available_at, temperature_alert_outbox_id)
    where status in ('pending', 'retry_wait', 'processing');

alter table public.temperature_alert_state enable row level security;
alter table public.temperature_alert_outbox enable row level security;

revoke all on table public.temperature_alert_state
    from public, anon, authenticated;
revoke all on table public.temperature_alert_outbox
    from public, anon, authenticated;
revoke all on sequence public.temperature_alert_incident_id_seq
    from public, anon, authenticated;
grant select, insert, update, delete on table
    public.temperature_alert_state to service_role;
grant select, insert, update, delete on table
    public.temperature_alert_outbox to service_role;
grant usage, select on sequence
    public.temperature_alert_incident_id_seq to service_role;

create function public.capture_temperature_alert_transition()
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
                new."sensorValue",
                v_sensor."setTmpUpLimit",
                new."sensorvaluetime"
            )
            on conflict do nothing;
        end if;
        return new;
    end if;

    if v_state.is_high = v_is_high then
        update public.temperature_alert_state
        set last_sensor_value_id = new."sensorValueId",
            last_value = new."sensorValue",
            last_upper_limit = v_sensor."setTmpUpLimit",
            updated_at = v_now
        where user_sensor_pk = v_sensor."Id";
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

create trigger trg_capture_temperature_alert_transition
after insert on public.sensorvalue
for each row
execute function public.capture_temperature_alert_transition();

create function public.drain_temperature_alert_outbox(
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
                        || ':' || v_item.event_key,
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

revoke all on function
    public.drain_temperature_alert_outbox(integer)
    from public, anon, authenticated;
grant execute on function
    public.drain_temperature_alert_outbox(integer)
    to service_role;

commit;
