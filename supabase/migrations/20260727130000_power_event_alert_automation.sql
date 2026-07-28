begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

do $create_event_gateway_secret$
declare
    v_source_secret text;
    v_secret_id uuid;
begin
    if exists (
        select 1
        from vault.decrypted_secrets as s
        where s.name = 'nb_iot_event_gateway_secret'
    ) then
        raise exception 'power event gateway secret already exists';
    end if;

    select s.decrypted_secret
    into strict v_source_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_config_request_secret';

    if nullif(v_source_secret, '') is null then
        raise exception 'power event gateway source secret is unavailable';
    end if;

    v_secret_id := vault.create_secret(
        v_source_secret,
        'nb_iot_event_gateway_secret',
        'NB-IOT power event gateway migration 20260727130000',
        null
    );

    perform vault.update_secret(
        v_secret_id,
        null,
        null,
        'NB-IOT power event gateway migration 20260727130000;secret_id='
            || v_secret_id::text,
        null
    );
end;
$create_event_gateway_secret$;

create table public.device_power_event (
    device_power_event_id bigint generated always as identity primary key,
    source_device_id integer not null
        references public.device ("deviceId") on delete restrict,
    device_imei text not null,
    source_origin text not null default 'mqtt',
    schema_version smallint not null,
    event_type smallint not null,
    incident_id bigint not null,
    event_sequence smallint not null,
    power_state smallint not null,
    value0 integer not null,
    value1 integer not null,
    device_unix_seconds bigint not null,
    clock_valid boolean not null,
    raw_payload jsonb not null,
    received_at timestamptz not null default statement_timestamp(),
    constraint device_power_event_imei_check
        check (device_imei ~ '^[0-9]{10,20}$'),
    constraint device_power_event_origin_check
        check (source_origin = 'mqtt'),
    constraint device_power_event_schema_check
        check (schema_version = 1),
    constraint device_power_event_identity_check
        check (
            event_type between 4 and 6
            and incident_id between 1 and 4294967295
            and event_sequence between 1 and 2
        ),
    constraint device_power_event_clock_check
        check (
            device_unix_seconds between 0 and 4294967295
            and (clock_valid or device_unix_seconds = 0)
        ),
    constraint device_power_event_payload_check
        check (
            pg_catalog.jsonb_typeof(raw_payload) = 'array'
            and pg_catalog.jsonb_array_length(raw_payload) = 9
        ),
    constraint device_power_event_canonical_check
        check (
            (
                event_type = 4
                and event_sequence = 1
                and power_state = 1
                and value0 = 0
                and value1 = 0
            )
            or (
                event_type = 5
                and event_sequence = 2
                and power_state = 0
                and value0 = 1
                and value1 = 0
            )
            or (
                event_type = 6
                and event_sequence = 2
                and power_state = 2
                and value0 = 210
                and value1 = 90
            )
        ),
    constraint device_power_event_idempotency_unique
        unique (
            source_device_id,
            source_origin,
            event_type,
            incident_id,
            event_sequence
        )
);

alter table public.device_power_event enable row level security;
revoke all on table public.device_power_event
    from public, anon, authenticated;
grant select, insert on table public.device_power_event to service_role;
revoke all on sequence public.device_power_event_device_power_event_id_seq
    from public, anon, authenticated;
grant usage, select
    on sequence public.device_power_event_device_power_event_id_seq
    to service_role;

create function public.ingest_device_power_event(
    p_imei text,
    p_schema smallint,
    p_event_type smallint,
    p_incident_id bigint,
    p_sequence smallint,
    p_state smallint,
    p_value0 integer,
    p_value1 integer,
    p_unix_seconds bigint,
    p_clock_valid boolean,
    p_request_secret text
)
returns jsonb
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_expected_request_secret text;
    v_device_count integer;
    v_device_id integer;
    v_user_id integer;
    v_event_id bigint;
    v_inserted boolean := false;
    v_message_queued boolean := false;
    v_event_key text;
    v_template_code text;
    v_now timestamptz;
    v_event_time text;
    v_contact_count integer;
    v_contact_id bigint;
    v_policy_count integer;
    v_policy public.message_policy%rowtype;
    v_workplace_name text;
    v_machine_count integer;
    v_machine_name text;
    v_history_token text;
    v_settings_token text;
    v_msg_send_id bigint;
begin
    if p_imei is null
       or p_imei !~ '^[0-9]{10,20}$'
       or p_schema is distinct from 1
       or p_event_type is null
       or p_incident_id is null
       or p_incident_id not between 1 and 4294967295
       or p_sequence is null
       or p_state is null
       or p_value0 is null
       or p_value1 is null
       or p_unix_seconds is null
       or p_unix_seconds not between 0 and 4294967295
       or p_clock_valid is null
       or (not p_clock_valid and p_unix_seconds <> 0)
       or p_request_secret is null
       or not (
           (
               p_event_type = 4 and p_sequence = 1 and p_state = 1
               and p_value0 = 0 and p_value1 = 0
           )
           or (
               p_event_type = 5 and p_sequence = 2 and p_state = 0
               and p_value0 = 1 and p_value1 = 0
           )
           or (
               p_event_type = 6 and p_sequence = 2 and p_state = 2
               and p_value0 = 210 and p_value1 = 90
           )
       ) then
        raise exception using
            errcode = '22023',
            message = 'invalid power event input';
    end if;

    select s.decrypted_secret
    into v_expected_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_event_gateway_secret';

    if v_expected_request_secret is null
       or p_request_secret is distinct from v_expected_request_secret then
        raise exception using
            errcode = '42501',
            message = 'power event gateway authorization failed';
    end if;

    select
        pg_catalog.count(*)::integer,
        min(d."deviceId"),
        min(d."userId")
    into v_device_count, v_device_id, v_user_id
    from public.device as d
    where d."deviceIMEI" = p_imei;

    if v_device_count <> 1
       or v_device_id is null
       or v_user_id is null then
        raise exception using
            errcode = '22023',
            message = 'power event device identity is not unique';
    end if;

    insert into public.device_power_event (
        source_device_id,
        device_imei,
        source_origin,
        schema_version,
        event_type,
        incident_id,
        event_sequence,
        power_state,
        value0,
        value1,
        device_unix_seconds,
        clock_valid,
        raw_payload
    )
    values (
        v_device_id,
        p_imei,
        'mqtt',
        p_schema,
        p_event_type,
        p_incident_id,
        p_sequence,
        p_state,
        p_value0,
        p_value1,
        p_unix_seconds,
        p_clock_valid,
        pg_catalog.jsonb_build_array(
            p_schema,
            p_event_type,
            p_incident_id,
            p_sequence,
            p_state,
            p_value0,
            p_value1,
            p_unix_seconds,
            p_clock_valid::integer
        )
    )
    on conflict on constraint device_power_event_idempotency_unique
    do nothing
    returning device_power_event_id into v_event_id;

    if v_event_id is not null then
        v_inserted := true;
    else
        select e.device_power_event_id
        into strict v_event_id
        from public.device_power_event as e
        where e.source_device_id = v_device_id
          and e.source_origin = 'mqtt'
          and e.event_type = p_event_type
          and e.incident_id = p_incident_id
          and e.event_sequence = p_sequence;
    end if;

    if v_inserted then
        begin
            v_now := pg_catalog.clock_timestamp();
            v_event_key := case p_event_type
                when 4 then 'adapter_removed'
                when 5 then 'adapter_restored'
                when 6 then 'power_shutdown'
            end;
            v_template_code := case p_event_type
                when 4 then 'bizp_2026071315030116762724896'
                when 5 then 'bizp_2026071315101276625891453'
                when 6 then 'bizp_2026071315073916762730996'
            end;
            v_event_time := case
                when p_clock_valid then
                    pg_catalog.to_char(
                        pg_catalog.to_timestamp(p_unix_seconds)
                            at time zone 'Asia/Seoul',
                        'YYYY-MM-DD HH24:MI:SS'
                    )
                else
                    pg_catalog.to_char(
                        v_now at time zone 'Asia/Seoul',
                        'YYYY-MM-DD HH24:MI:SS'
                    )
            end;

            select pg_catalog.count(*)::integer, min(c.alert_contact_id)
            into v_contact_count, v_contact_id
            from public.alert_contact as c
            where c.user_id = v_user_id
              and c.is_active
              and c.consent_status = 'granted'
              and c.consent_revoked_at is null;

            select pg_catalog.count(*)::integer
            into v_policy_count
            from public.message_policy as p
            where p.is_active
              and p.primary_channel = 'alimtalk'
              and not p.allow_sms_fallback
              and p.effective_from <= v_now
              and (
                  p.effective_until is null
                  or p.effective_until > v_now
              );

            if v_contact_count <> 1 or v_policy_count <> 1 then
                raise exception 'power alert contact or policy cardinality';
            end if;

            select p.*
            into strict v_policy
            from public.message_policy as p
            where p.is_active
              and p.primary_channel = 'alimtalk'
              and not p.allow_sms_fallback
              and p.effective_from <= v_now
              and (
                  p.effective_until is null
                  or p.effective_until > v_now
              )
            for share of p;

            select w."WorkplaceName"
            into v_workplace_name
            from public.device as d
            join public.userworkplace as w
              on w."userWorkplaceId" = d."userWorkplaceId"
             and w."userId" = d."userId"
            where d."deviceId" = v_device_id
              and d."userId" = v_user_id
            for share of d, w;

            select
                pg_catalog.count(*)::integer,
                min(m."userMachineName")
            into v_machine_count, v_machine_name
            from public.usermachine as m
            where m."deviceId" = v_device_id;

            if v_workplace_name is null
               or pg_catalog.length(pg_catalog.btrim(v_workplace_name))
                      not between 1 and 512
               or position('#{' in v_workplace_name) > 0
               or v_machine_count <> 1
               or v_machine_name is null
               or pg_catalog.length(pg_catalog.btrim(v_machine_name))
                      not between 1 and 512
               or position('#{' in v_machine_name) > 0 then
                raise exception 'power alert message identity is invalid';
            end if;

            v_history_token :=
                pg_catalog.replace(
                    pg_catalog.gen_random_uuid()::text,
                    '-',
                    ''
                )
                || pg_catalog.replace(
                    pg_catalog.gen_random_uuid()::text,
                    '-',
                    ''
                );
            v_settings_token :=
                pg_catalog.replace(
                    pg_catalog.gen_random_uuid()::text,
                    '-',
                    ''
                )
                || pg_catalog.replace(
                    pg_catalog.gen_random_uuid()::text,
                    '-',
                    ''
                );

            insert into public.msg_send (
                alert_contact_id,
                message_policy_id,
                source_user_id,
                source_device_id,
                source_event_origin,
                source_event_type,
                source_event_sequence,
                incident_id,
                incident_kind,
                template_code,
                template_stage,
                template_params,
                dedupe_key,
                priority,
                available_at,
                expires_at,
                max_attempts
            )
            values (
                v_contact_id,
                v_policy.message_policy_id,
                v_user_id,
                v_device_id,
                'device_power_event',
                v_event_key,
                p_sequence,
                p_incident_id,
                v_event_key,
                v_template_code,
                v_event_key,
                pg_catalog.jsonb_build_object(
                    'workplaceName', pg_catalog.btrim(v_workplace_name),
                    'userMachineName', pg_catalog.btrim(v_machine_name),
                    'eventTime', v_event_time,
                    'tempHistoryToken', v_history_token,
                    'SettingsToken', v_settings_token
                ),
                'power_event:' || v_device_id::text || ':'
                    || p_event_type::text || ':' || p_incident_id::text
                    || ':' || p_sequence::text,
                case when p_event_type = 6 then 100 else 90 end,
                v_now,
                v_now + interval '24 hours',
                v_policy.max_attempts
            )
            returning msg_send_id into strict v_msg_send_id;

            insert into public.message_link_token (
                msg_send_id,
                token_hash,
                purpose,
                target_path,
                expires_at
            )
            values
                (
                    v_msg_send_id,
                    extensions.digest(v_history_token, 'sha256'),
                    'temperature_history',
                    '/history?deviceId=' || v_device_id::text,
                    v_now + interval '24 hours'
                ),
                (
                    v_msg_send_id,
                    extensions.digest(v_settings_token, 'sha256'),
                    'device_settings',
                    '/device-settings/' || v_device_id::text,
                    v_now + interval '24 hours'
                );
            v_message_queued := true;
        exception when others then
            raise warning 'power event alert enqueue failed [%]', SQLSTATE;
            v_message_queued := false;
        end;
    end if;

    return pg_catalog.jsonb_build_object(
        'eventId', v_event_id,
        'duplicate', not v_inserted,
        'messageQueued', v_message_queued
    );
end;
$function$;

revoke all on function public.ingest_device_power_event(
    text,
    smallint,
    smallint,
    bigint,
    smallint,
    smallint,
    integer,
    integer,
    bigint,
    boolean,
    text
) from public, anon, authenticated;
grant execute on function public.ingest_device_power_event(
    text,
    smallint,
    smallint,
    bigint,
    smallint,
    smallint,
    integer,
    integer,
    bigint,
    boolean,
    text
) to anon, service_role;

commit;
