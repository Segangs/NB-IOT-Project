begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

create function public.capture_device_boot_alert()
returns trigger
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_now timestamptz;
    v_contact_id bigint;
    v_contact_count integer;
    v_policy public.message_policy%rowtype;
    v_policy_count integer;
    v_workplace_name text;
    v_machine_name text;
    v_machine_count integer;
    v_history_token text;
    v_settings_token text;
    v_msg_send_id bigint;
begin
    -- Only the established EMQX -> PostgREST boot RPC may create alerts.
    -- Direct REST table inserts and SQL maintenance inserts remain silent.
    if nullif(current_setting('request.path',true),'')
           is distinct from '/rpc/b'
       or nullif(current_setting('request.method',true),'')
           is distinct from 'POST' then
        return new;
    end if;

    -- A boot alert means the core self-test and modem attachment completed.
    -- Sensor-channel status is intentionally not part of this decision.
    if new."userId" is null
       or new."deviceId" is null
       or new.id is null
       or new.boottime is null
       or pg_catalog.btrim(new.boottime) = ''
       or new.flash_integrity is distinct from 0
       or new.ram_test is distinct from 0
       or new.at_status is distinct from 0
       or new.cpin_status is distinct from 0 then
        return new;
    end if;

    v_now := pg_catalog.clock_timestamp();

    -- Serialize cooldown checks per device so concurrent boot rows cannot
    -- enqueue more than one message.
    perform pg_catalog.pg_advisory_xact_lock(
        pg_catalog.hashtextextended(
            'device_boot:' || new."deviceId"::text,
            0
        )
    );

    if exists (
        select 1
        from public.msg_send as m
        where m.source_device_id = new."deviceId"
          and m.source_event_type = 'device_boot'
          and m.created_at >= v_now - interval '5 minutes'
    ) then
        return new;
    end if;

    select pg_catalog.count(*)::integer, min(c.alert_contact_id)
    into v_contact_count, v_contact_id
    from public.alert_contact as c
    where c.user_id = new."userId"
      and c.is_active
      and c.consent_status = 'granted'
      and c.consent_revoked_at is null;
    if v_contact_count <> 1 then
        return new;
    end if;

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
    if v_policy_count <> 1 then
        return new;
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
    where d."deviceId" = new."deviceId"
      and d."userId" = new."userId"
    for share of d, w;

    select pg_catalog.count(*)::integer, min(m."userMachineName")
    into v_machine_count, v_machine_name
    from public.usermachine as m
    where m."deviceId" = new."deviceId";

    if v_workplace_name is null
       or pg_catalog.length(pg_catalog.btrim(v_workplace_name))
              not between 1 and 512
       or position('#{' in v_workplace_name) > 0
       or v_machine_count <> 1
       or v_machine_name is null
       or pg_catalog.length(pg_catalog.btrim(v_machine_name))
              not between 1 and 512
       or position('#{' in v_machine_name) > 0
       or pg_catalog.length(pg_catalog.btrim(new.boottime))
              not between 1 and 64
       or position('#{' in new.boottime) > 0 then
        return new;
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
        new."userId",
        new."deviceId",
        'device_boot_logs',
        'device_boot',
        new.id,
        new.id,
        'device_boot',
        'bizp_2026071315003676625784727',
        'device_boot',
        pg_catalog.jsonb_build_object(
            'workplaceName', v_workplace_name,
            'userMachineName', v_machine_name,
            'eventTime', pg_catalog.btrim(new.boottime),
            'tempHistoryToken', v_history_token,
            'SettingsToken', v_settings_token
        ),
        'device_boot:' || new."deviceId"::text
            || ':' || new.id::text,
        50,
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
            extensions.digest(v_history_token,'sha256'),
            'temperature_history',
            '/history?deviceId=' || new."deviceId"::text,
            v_now + interval '24 hours'
        ),
        (
            v_msg_send_id,
            extensions.digest(v_settings_token,'sha256'),
            'device_settings',
            '/device-settings/' || new."deviceId"::text,
            v_now + interval '24 hours'
        );

    return new;
exception when others then
    -- Alert generation must never reject or roll back the boot log.
    raise warning 'device boot alert enqueue failed [%]',SQLSTATE;
    return new;
end;
$function$;

revoke all on function public.capture_device_boot_alert()
    from public, anon, authenticated;
grant execute on function public.capture_device_boot_alert()
    to service_role;

create trigger trg_capture_device_boot_alert
after insert on public.device_boot_logs
for each row
execute function public.capture_device_boot_alert();

commit;
