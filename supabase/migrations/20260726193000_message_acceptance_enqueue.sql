begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

-- Provider acceptance mode deliberately records Bizppurio code 1000 as the
-- success basis. It does not fabricate delivery callback code 7000.
create function public.finalize_msg_send_provider_acceptance(
    p_msg_send_id bigint,
    p_lock_owner text,
    p_lease_token uuid,
    p_channel text,
    p_provider_request_id text,
    p_accepted_at timestamptz,
    p_cost_amount numeric
)
returns table (
    applied boolean,
    duplicate boolean,
    message jsonb
)
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_registration record;
    v_result record;
    v_policy public.message_policy%rowtype;
begin
    if p_channel is distinct from 'alimtalk' then
        raise exception 'provider acceptance mode is alimtalk only'
            using errcode = '22023';
    end if;

    select r.*
    into strict v_registration
    from public.mark_msg_send_submission_waiting_result(
        p_msg_send_id,
        p_lock_owner,
        p_lease_token,
        p_channel,
        p_provider_request_id,
        p_accepted_at
    ) as r;

    select p.*
    into strict v_policy
    from public.msg_send as m
    join public.message_policy as p
      on p.message_policy_id = m.message_policy_id
    where m.msg_send_id = p_msg_send_id;

    select r.*
    into strict v_result
    from public.record_msg_send_push_result(
        p_provider_request_id => p_provider_request_id,
        p_submission_lease_token => p_lease_token,
        p_provider_result_id =>
            'acceptance:' || p_lease_token::text,
        p_channel => p_channel,
        p_delivered => true,
        p_retryable => false,
        p_result_at => p_accepted_at,
        p_result_code => '1000',
        p_cost_amount => p_cost_amount,
        p_lock_owner => p_lock_owner,
        p_lease_seconds => greatest(v_policy.lease_seconds, 30),
        p_max_attempts => v_policy.max_attempts,
        p_retry_base_seconds => v_policy.retry_base_seconds,
        p_retry_max_seconds => v_policy.retry_max_seconds
    ) as r;

    if v_result.action not in ('delivered', 'duplicate') then
        raise exception 'provider acceptance produced unexpected action'
            using errcode = '55000';
    end if;

    return query
    select
        v_result.applied,
        v_result.duplicate,
        v_result.message;
end;
$function$;

revoke all on function public.finalize_msg_send_provider_acceptance(
    bigint,text,uuid,text,text,timestamptz,numeric
) from public, anon, authenticated;
grant execute on function public.finalize_msg_send_provider_acceptance(
    bigint,text,uuid,text,text,timestamptz,numeric
) to service_role;

-- This producer boundary owns the raw opaque tokens only long enough to put
-- them in the provider template parameters. Link rows retain SHA-256 only.
create function public.enqueue_temp_alert_msg_send(
    p_alert_contact_id bigint,
    p_message_policy_id bigint,
    p_source_user_id integer,
    p_source_device_id integer,
    p_source_event_origin text,
    p_source_event_sequence bigint,
    p_incident_id bigint,
    p_event_key text,
    p_template_params jsonb,
    p_dedupe_key text,
    p_expires_at timestamptz,
    p_priority smallint default 0
)
returns bigint
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_now timestamptz;
    v_contact public.alert_contact%rowtype;
    v_policy public.message_policy%rowtype;
    v_device_user_id integer;
    v_device_workplace_id integer;
    v_template_code text;
    v_template_stage text;
    v_existing public.msg_send%rowtype;
    v_link_count integer;
    v_history_count integer;
    v_settings_count integer;
    v_message_id bigint;
    v_history_token text;
    v_settings_token text;
begin
    v_now := pg_catalog.clock_timestamp();

    if p_alert_contact_id is null or p_alert_contact_id <= 0
       or p_message_policy_id is null or p_message_policy_id <= 0
       or p_source_user_id is null or p_source_user_id <= 0
       or p_source_device_id is null or p_source_device_id <= 0
       or p_source_event_sequence is null
       or p_source_event_sequence < 0
       or p_incident_id is null or p_incident_id <= 0 then
        raise exception 'invalid TEMP alert identity'
            using errcode = '22023';
    end if;
    if p_source_event_origin is null
       or pg_catalog.length(
           pg_catalog.btrim(p_source_event_origin)
       ) not between 1 and 64
       or p_dedupe_key is null
       or pg_catalog.length(p_dedupe_key) not between 1 and 256 then
        raise exception 'invalid TEMP alert provenance'
            using errcode = '22023';
    end if;
    if p_expires_at is null
       or p_expires_at <= v_now
       or p_expires_at > v_now + interval '7 days'
       or p_priority is null
       or p_priority not between 0 and 100 then
        raise exception 'invalid TEMP alert queue bounds'
            using errcode = '22023';
    end if;
    if p_template_params is null
       or pg_catalog.jsonb_typeof(p_template_params) <> 'object'
       or p_template_params ? 'tempHistoryToken'
       or p_template_params ? 'SettingsToken' then
        raise exception 'invalid TEMP template parameters'
            using errcode = '22023';
    end if;

    if p_event_key = 'temperature_high' then
        v_template_code := 'bizp_2026070914231016762370714';
        v_template_stage := 'temperature_high';
        if (
               select pg_catalog.count(*)
               from pg_catalog.jsonb_object_keys(p_template_params)
           ) <> 5
           or not (
               p_template_params ?& array[
                   'sensorValue',
                   'WorkplaceName',
                   'userMachineName',
                   'setTmpUpLimit',
                   'sensorvaluetime'
               ]
           ) then
            raise exception 'temperature_high parameters mismatch'
                using errcode = '22023';
        end if;
    elsif p_event_key = 'temperature_recovered' then
        v_template_code := 'bizp_2026071314514616762878062';
        v_template_stage := 'temperature_recovered';
        if (
               select pg_catalog.count(*)
               from pg_catalog.jsonb_object_keys(p_template_params)
           ) <> 4
           or not (
               p_template_params ?& array[
                   'sensorValue',
                   'workplaceName',
                   'userMachineName',
                   'setTmpUpLimit'
               ]
           ) then
            raise exception 'temperature_recovered parameters mismatch'
                using errcode = '22023';
        end if;
    else
        raise exception 'unsupported TEMP event key'
            using errcode = '22023';
    end if;

    if exists (
        select 1
        from pg_catalog.jsonb_each(p_template_params) as item(key, value)
        where pg_catalog.jsonb_typeof(item.value)
                  not in ('string', 'number')
           or pg_catalog.length(item.value #>> '{}') not between 1 and 512
           or position('#{' in (item.value #>> '{}')) > 0
    ) then
        raise exception 'unsafe TEMP template parameter value'
            using errcode = '22023';
    end if;

    select c.*
    into strict v_contact
    from public.alert_contact as c
    where c.alert_contact_id = p_alert_contact_id
    for share of c;
    if v_contact.user_id is distinct from p_source_user_id
       or not v_contact.is_active
       or v_contact.consent_status is distinct from 'granted'
       or v_contact.consent_revoked_at is not null then
        raise exception 'TEMP alert contact is not authorized'
            using errcode = '42501';
    end if;

    select p.*
    into strict v_policy
    from public.message_policy as p
    where p.message_policy_id = p_message_policy_id
    for share of p;
    if not v_policy.is_active
       or v_policy.primary_channel is distinct from 'alimtalk'
       or v_policy.allow_sms_fallback
       or v_policy.effective_from > v_now
       or (
           v_policy.effective_until is not null
           and v_policy.effective_until <= v_now
       ) then
        raise exception 'TEMP message policy is not active'
            using errcode = '42501';
    end if;

    select d."userId", d."userWorkplaceId"
    into strict v_device_user_id, v_device_workplace_id
    from public.device as d
    where d."deviceId" = p_source_device_id
    for share of d;
    if v_device_user_id is distinct from p_source_user_id
       or v_device_workplace_id is null then
        raise exception 'TEMP alert device ownership mismatch'
            using errcode = '42501';
    end if;

    perform 1
    from public.userworkplace as w
    where w."userWorkplaceId" = v_device_workplace_id
      and w."userId" = p_source_user_id
    for share of w;
    if not found then
        raise exception 'TEMP alert device ownership mismatch'
            using errcode = '42501';
    end if;

    perform 1
    from public."USER_SENSOR" as s
    join public."SENSOR_CTGY" as c
      on c."sensorCtgyId" = s."sensorCtgyId"
    where s."deviceId" = p_source_device_id
      and c."sensorCtgyType" = 'TMP'
    for share of s, c;
    if not found then
        raise exception 'TEMP alert device has no temperature sensor'
            using errcode = '22023';
    end if;

    perform pg_catalog.pg_advisory_xact_lock(
        pg_catalog.hashtextextended(p_dedupe_key, 0)
    );

    select m.*
    into v_existing
    from public.msg_send as m
    where m.dedupe_key = p_dedupe_key;

    if found then
        select
            pg_catalog.count(*)::integer,
            pg_catalog.count(*) filter (
                where l.purpose = 'temperature_history'
                  and l.target_path =
                      '/history?deviceId=' || p_source_device_id::text
                  and l.expires_at = p_expires_at
                  and l.token_hash = extensions.digest(
                      v_existing.template_params
                          ->> 'tempHistoryToken',
                      'sha256'
                  )
            )::integer,
            pg_catalog.count(*) filter (
                where l.purpose = 'device_settings'
                  and l.target_path =
                      '/device-settings/' || p_source_device_id::text
                  and l.expires_at = p_expires_at
                  and l.token_hash = extensions.digest(
                      v_existing.template_params
                          ->> 'SettingsToken',
                      'sha256'
                  )
            )::integer
        into v_link_count, v_history_count, v_settings_count
        from public.message_link_token as l
        where l.msg_send_id = v_existing.msg_send_id;

        if v_existing.alert_contact_id
               is distinct from p_alert_contact_id
           or v_existing.message_policy_id
               is distinct from p_message_policy_id
           or v_existing.source_user_id
               is distinct from p_source_user_id
           or v_existing.source_device_id
               is distinct from p_source_device_id
           or v_existing.source_event_origin
               is distinct from p_source_event_origin
           or v_existing.source_event_type is distinct from p_event_key
           or v_existing.source_event_sequence
               is distinct from p_source_event_sequence
           or v_existing.incident_id is distinct from p_incident_id
           or v_existing.incident_kind is distinct from 'temperature'
           or v_existing.template_code is distinct from v_template_code
           or v_existing.template_stage is distinct from v_template_stage
           or (
               v_existing.template_params
                   - 'tempHistoryToken'
                   - 'SettingsToken'
           ) is distinct from p_template_params
           or v_existing.priority is distinct from p_priority
           or v_existing.expires_at is distinct from p_expires_at
           or v_link_count <> 2
           or v_history_count <> 1
           or v_settings_count <> 1 then
            raise exception 'enqueue replay mismatch'
                using errcode = '22000';
        end if;
        return v_existing.msg_send_id;
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
        p_alert_contact_id,
        p_message_policy_id,
        p_source_user_id,
        p_source_device_id,
        p_source_event_origin,
        p_event_key,
        p_source_event_sequence,
        p_incident_id,
        'temperature',
        v_template_code,
        v_template_stage,
        p_template_params
            || pg_catalog.jsonb_build_object(
                'tempHistoryToken',
                v_history_token,
                'SettingsToken',
                v_settings_token
            ),
        p_dedupe_key,
        p_priority,
        v_now,
        p_expires_at,
        v_policy.max_attempts
    )
    returning msg_send_id into strict v_message_id;

    insert into public.message_link_token (
        msg_send_id,
        token_hash,
        purpose,
        target_path,
        expires_at
    )
    values
        (
            v_message_id,
            extensions.digest(v_history_token, 'sha256'),
            'temperature_history',
            '/history?deviceId=' || p_source_device_id::text,
            p_expires_at
        ),
        (
            v_message_id,
            extensions.digest(v_settings_token, 'sha256'),
            'device_settings',
            '/device-settings/' || p_source_device_id::text,
            p_expires_at
        );

    return v_message_id;
end;
$function$;

revoke all on function public.enqueue_temp_alert_msg_send(
    bigint,bigint,integer,integer,text,bigint,bigint,text,jsonb,text,
    timestamptz,smallint
) from public, anon, authenticated;
grant execute on function public.enqueue_temp_alert_msg_send(
    bigint,bigint,integer,integer,text,bigint,bigint,text,jsonb,text,
    timestamptz,smallint
) to service_role;

commit;
