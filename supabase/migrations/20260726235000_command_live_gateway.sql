begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.sensorvalue in share row exclusive mode;
lock table public."deviceCmds" in share row exclusive mode;

do $gateway_cutover_guard$
declare
    v_assign_md5 text;
    v_legacy_trigger_count integer;
    v_required_secret_count integer;
    v_sync_trigger_count integer;
begin
    if to_regclass('public.device_command_state') is null
       or to_regprocedure(
           'public.claim_device_command(text,bigint,bigint,integer)'
       ) is null
       or to_regprocedure(
           'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'
       ) is null then
        raise exception using
            errcode = '55000',
            message = 'command companion boundary is not installed';
    end if;

    if to_regclass('net.http_request_queue') is null
       or to_regprocedure(
        'net.http_post(text,jsonb,jsonb,jsonb,integer)'
    ) is null then
        raise exception using
            errcode = '55000',
            message = 'command publish transport is not installed';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        join pg_catalog.pg_namespace as n
          on n.oid = p.pronamespace
        where n.nspname = 'public'
          and p.proname in (
              'publish_device_command_response',
              'publish_device_command_ack_receipt'
          )
    ) then
        raise exception using
            errcode = '42710',
            message = 'command gateway target already exists';
    end if;

    select count(*)
    into v_legacy_trigger_count
    from pg_catalog.pg_trigger as t
    where t.tgrelid = 'public.sensorvalue'::regclass
      and t.tgname = 'trg_assign_device_command'
      and not t.tgisinternal
      and t.tgenabled = 'O'
      and t.tgtype = 7
      and t.tgfoid = 'public.assign_device_command()'::regprocedure
      and t.tgqual is null
      and t.tgnargs = 0;

    if v_legacy_trigger_count <> 1 then
        raise exception using
            errcode = '55000',
            message = 'legacy command trigger definition drift';
    end if;

    select md5(
        pg_catalog.pg_get_functiondef(
            'public.assign_device_command()'::regprocedure
        )
    )
    into v_assign_md5;

    if v_assign_md5 <> 'de97b37ad246c5e509ae06f821a10338' then
        raise exception using
            errcode = '55000',
            message = 'legacy command function definition drift';
    end if;

    select count(*)
    into v_sync_trigger_count
    from pg_catalog.pg_trigger as t
    where t.tgrelid = 'public."deviceCmds"'::regclass
      and t.tgname = 'trg_sync_device_command_state'
      and not t.tgisinternal
      and t.tgenabled = 'O'
      and t.tgtype = 21
      and t.tgfoid =
          'public.sync_device_command_state()'::regprocedure
      and t.tgqual is null
      and t.tgnargs = 0
      and t.tgattr::text = (
          select pg_catalog.string_agg(
              a.attnum::text,
              ' ' order by expected.ordinality
          )
          from (
              values
                  ('status', 1),
                  ('sent_at', 2),
                  ('cmd', 3),
                  ('deviceId', 4)
          ) as expected(attname, ordinality)
          join pg_catalog.pg_attribute as a
            on a.attrelid = 'public."deviceCmds"'::regclass
           and a.attname = expected.attname
           and not a.attisdropped
      );

    if v_sync_trigger_count <> 1 then
        raise exception using
            errcode = '55000',
            message = 'command state sync trigger definition drift';
    end if;

    if exists (
        select 1
        from public."deviceCmds"
        where status = 0
    ) then
        raise exception using
            errcode = '55000',
            message = 'pending legacy command blocks gateway cutover';
    end if;

    if to_regclass('vault.decrypted_secrets') is null
       or to_regclass('vault.secrets') is null
       or to_regprocedure(
           'vault.create_secret(text,text,text,uuid)'
       ) is null
       or not exists (
           select 1
           from pg_catalog.pg_proc as p
           where p.oid =
               'vault.create_secret(text,text,text,uuid)'::regprocedure
             and p.pronargs = 4
             and p.pronargdefaults = 3
       )
       or to_regprocedure(
           'vault.update_secret(uuid,text,text,text,uuid)'
       ) is null
       or not exists (
           select 1
           from pg_catalog.pg_proc as p
           where p.oid =
               'vault.update_secret(uuid,text,text,text,uuid)'::regprocedure
             and p.pronargs = 5
             and p.pronargdefaults = 4
       ) then
        raise exception using
            errcode = '55000',
            message = 'command gateway vault boundary is not installed';
    end if;

    select count(*)
    into v_required_secret_count
    from vault.decrypted_secrets as s
    where s.name in (
        'nb_iot_config_request_secret',
        'nb_iot_emqx_publish_url',
        'nb_iot_emqx_publish_key',
        'nb_iot_emqx_publish_secret'
    )
      and nullif(s.decrypted_secret, '') is not null;

    if v_required_secret_count <> 4 then
        raise exception using
            errcode = '55000',
            message = 'command gateway prerequisite secret is unavailable';
    end if;

    if exists (
        select 1
        from vault.decrypted_secrets as s
        where s.name = 'nb_iot_command_gateway_secret'
    ) then
        raise exception using
            errcode = '42710',
            message = 'command gateway secret alias already exists';
    end if;
end;
$gateway_cutover_guard$;

do $create_command_gateway_secret$
declare
    v_alias_provenance_count integer;
    v_config_request_secret text;
    v_gateway_secret_id uuid;
begin
    select s.decrypted_secret
    into strict v_config_request_secret
    from vault.decrypted_secrets as s
    where name = 'nb_iot_config_request_secret';

    if nullif(v_config_request_secret, '') is null then
        raise exception using
            errcode = '55000',
            message = 'command gateway source secret is unavailable';
    end if;

    v_gateway_secret_id := vault.create_secret(
        v_config_request_secret,
        'nb_iot_command_gateway_secret',
        'NB-IOT command gateway migration 20260726235000;pending',
        null
    );

    if v_gateway_secret_id is null then
        raise exception using
            errcode = '55000',
            message = 'command gateway secret alias creation failed';
    end if;

    perform vault.update_secret(
        v_gateway_secret_id,
        null,
        null,
        'NB-IOT command gateway migration 20260726235000;secret_id='
            || v_gateway_secret_id::text,
        null
    );

    select count(*)
    into v_alias_provenance_count
    from vault.decrypted_secrets as s
    join vault.secrets as d
      on s.id = d.id
    where s.id = v_gateway_secret_id
      and s.name = 'nb_iot_command_gateway_secret'
      and d.name = 'nb_iot_command_gateway_secret'
      and d.description =
          'NB-IOT command gateway migration 20260726235000;secret_id='
              || s.id::text
      and s.decrypted_secret is not distinct from
          v_config_request_secret;

    if v_alias_provenance_count <> 1 then
        raise exception using
            errcode = '55000',
            message = 'command gateway secret alias provenance failed';
    end if;
end;
$create_command_gateway_secret$;

create function public.publish_device_command_response(
    p_imei text,
    p_request_id bigint,
    p_last_cmd_id bigint,
    p_request_secret text
)
returns bigint
language plpgsql
security definer
set search_path = ''
as $function$
declare
    v_expected_request_secret text;
    v_emqx_publish_url text;
    v_emqx_publish_key text;
    v_emqx_publish_secret text;
    v_claim_receipt jsonb;
    v_receipt_request_id bigint;
    v_receipt_cmd_id bigint;
    v_receipt_opcode smallint;
    v_receipt_job_id bigint;
    v_receipt_ttl_seconds integer;
    v_payload text;
    v_http_request_id bigint;
begin
    if p_imei is null
       or p_imei !~ '^[0-9]{10,20}$'
       or p_request_id is null
       or p_request_id not between 1 and 4294967295
       or p_last_cmd_id is null
       or p_last_cmd_id not between 0 and 4294967295
       or p_request_secret is null then
        raise exception using
            errcode = '22023',
            message = 'invalid command request input';
    end if;

    select s.decrypted_secret
    into v_expected_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_command_gateway_secret';

    if v_expected_request_secret is null
       or p_request_secret is distinct from v_expected_request_secret then
        raise exception using
            errcode = '42501',
            message = 'command gateway authorization failed';
    end if;

    select public.claim_device_command(
        p_imei,
        p_request_id,
        p_last_cmd_id,
        60
    )
    into v_claim_receipt;

    if pg_catalog.jsonb_typeof(v_claim_receipt) is distinct from 'array' then
        raise exception using
            errcode = '55000',
            message = 'command claim returned an invalid receipt';
    end if;

    if pg_catalog.jsonb_array_length(v_claim_receipt) <> 5 then
        raise exception using
            errcode = '55000',
            message = 'command claim returned an invalid receipt';
    end if;

    if pg_catalog.jsonb_typeof(v_claim_receipt -> 0)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_claim_receipt -> 1)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_claim_receipt -> 2)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_claim_receipt -> 3)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_claim_receipt -> 4)
           is distinct from 'number' then
        raise exception using
            errcode = '55000',
            message = 'command claim returned an invalid receipt';
    end if;

    begin
        v_receipt_request_id := (v_claim_receipt ->> 0)::bigint;
        v_receipt_cmd_id := (v_claim_receipt ->> 1)::bigint;
        v_receipt_opcode := (v_claim_receipt ->> 2)::smallint;
        v_receipt_job_id := (v_claim_receipt ->> 3)::bigint;
        v_receipt_ttl_seconds := (v_claim_receipt ->> 4)::integer;
    exception
        when invalid_text_representation or numeric_value_out_of_range then
            raise exception using
                errcode = '55000',
                message = 'command claim returned an invalid receipt';
    end;

    if v_receipt_request_id is null
       or v_receipt_request_id is distinct from p_request_id
       or v_receipt_cmd_id is null
       or v_receipt_opcode is null
       or v_receipt_job_id is null
       or v_receipt_ttl_seconds is null
       or v_receipt_cmd_id not between 0 and 4294967295
       or (
           v_receipt_cmd_id = 0
           and (
               v_receipt_opcode <> 0
               or v_receipt_job_id <> 0
               or v_receipt_ttl_seconds <> 0
           )
       )
       or (
           v_receipt_cmd_id > 0
           and (
               v_receipt_opcode not between 1 and 4
               or v_receipt_job_id not between 1 and 4294967295
               or v_receipt_ttl_seconds not between 1 and 86400
           )
       ) then
        raise exception using
            errcode = '55000',
            message = 'command claim returned an invalid receipt';
    end if;

    v_payload := pg_catalog.replace(
        v_claim_receipt::text,
        ' ',
        ''
    );

    if pg_catalog.octet_length(v_payload) > 80 then
        raise exception using
            errcode = '54000',
            message = 'command response payload exceeds modem limit';
    end if;

    select
        (
            select s.decrypted_secret
            from vault.decrypted_secrets as s
            where s.name = 'nb_iot_emqx_publish_url'
        ),
        (
            select s.decrypted_secret
            from vault.decrypted_secrets as s
            where s.name = 'nb_iot_emqx_publish_key'
        ),
        (
            select s.decrypted_secret
            from vault.decrypted_secrets as s
            where s.name = 'nb_iot_emqx_publish_secret'
        )
    into
        v_emqx_publish_url,
        v_emqx_publish_key,
        v_emqx_publish_secret;

    if nullif(v_emqx_publish_url, '') is null
       or nullif(v_emqx_publish_key, '') is null
       or nullif(v_emqx_publish_secret, '') is null then
        raise exception using
            errcode = '55000',
            message = 'command publish destination is not configured';
    end if;

    select net.http_post(
        url := v_emqx_publish_url,
        headers := pg_catalog.jsonb_build_object(
            'Content-Type',
            'application/json',
            'Authorization', 'Basic ' || pg_catalog.replace(
                pg_catalog.encode(
                    pg_catalog.convert_to(
                        v_emqx_publish_key || ':' || v_emqx_publish_secret,
                        'UTF8'
                    ),
                    'base64'
                ),
                E'\n',
                ''
            )
        ),
        body := pg_catalog.jsonb_build_object(
            'topic',
            'devices/' || p_imei || '/cmd/response',
            'qos', 1,
            'retain', false,
            'payload',
            v_payload
        ),
        timeout_milliseconds := 5000
    )
    into v_http_request_id;

    if v_http_request_id is null then
        raise exception using
            errcode = '55000',
            message = 'command publish request was not queued';
    end if;

    return v_http_request_id;
end;
$function$;

create function public.publish_device_command_ack_receipt(
    p_imei text,
    p_cmd_id bigint,
    p_phase smallint,
    p_result smallint,
    p_error smallint,
    p_unix_seconds bigint,
    p_clock_valid boolean,
    p_request_secret text
)
returns bigint
language plpgsql
security definer
set search_path = ''
as $function$
declare
    v_expected_request_secret text;
    v_emqx_publish_url text;
    v_emqx_publish_key text;
    v_emqx_publish_secret text;
    v_ack_receipt jsonb;
    v_receipt_cmd_id bigint;
    v_receipt_phase smallint;
    v_receipt_result smallint;
    v_receipt_status smallint;
    v_receipt_error smallint;
    v_payload text;
    v_http_request_id bigint;
begin
    if p_imei is null
       or p_imei !~ '^[0-9]{10,20}$'
       or p_cmd_id is null
       or p_cmd_id not between 1 and 4294967295
       or p_phase is null
       or p_phase not between 1 and 2
       or p_result is null
       or p_result not between 1 and 4
       or p_error is null
       or p_error not between 0 and 5
       or p_unix_seconds is null
       or p_unix_seconds not between 0 and 4294967295
       or p_clock_valid is null
       or p_request_secret is null then
        raise exception using
            errcode = '22023',
            message = 'invalid command acknowledgement input';
    end if;

    select s.decrypted_secret
    into v_expected_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_command_gateway_secret';

    if v_expected_request_secret is null
       or p_request_secret is distinct from v_expected_request_secret then
        raise exception using
            errcode = '42501',
            message = 'command gateway authorization failed';
    end if;

    select public.ack_device_command(
        p_imei,
        p_cmd_id,
        p_phase,
        p_result,
        p_error,
        p_unix_seconds,
        p_clock_valid
    )
    into v_ack_receipt;

    if pg_catalog.jsonb_typeof(v_ack_receipt) is distinct from 'array' then
        raise exception using
            errcode = '55000',
            message = 'command acknowledgement returned an invalid receipt';
    end if;

    if pg_catalog.jsonb_array_length(v_ack_receipt) <> 5 then
        raise exception using
            errcode = '55000',
            message = 'command acknowledgement returned an invalid receipt';
    end if;

    if pg_catalog.jsonb_typeof(v_ack_receipt -> 0)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_ack_receipt -> 1)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_ack_receipt -> 2)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_ack_receipt -> 3)
           is distinct from 'number'
       or pg_catalog.jsonb_typeof(v_ack_receipt -> 4)
           is distinct from 'number' then
        raise exception using
            errcode = '55000',
            message = 'command acknowledgement returned an invalid receipt';
    end if;

    begin
        v_receipt_cmd_id := (v_ack_receipt ->> 0)::bigint;
        v_receipt_phase := (v_ack_receipt ->> 1)::smallint;
        v_receipt_result := (v_ack_receipt ->> 2)::smallint;
        v_receipt_status := (v_ack_receipt ->> 3)::smallint;
        v_receipt_error := (v_ack_receipt ->> 4)::smallint;
    exception
        when invalid_text_representation or numeric_value_out_of_range then
            raise exception using
                errcode = '55000',
                message = 'command acknowledgement returned an invalid receipt';
    end;

    if v_receipt_cmd_id is null
       or v_receipt_phase is null
       or v_receipt_result is null
       or v_receipt_status is null
       or v_receipt_error is null
       or v_receipt_cmd_id is distinct from p_cmd_id
       or v_receipt_phase is distinct from p_phase
       or v_receipt_result is distinct from p_result
       or v_receipt_status not between 1 and 3
       or v_receipt_error not between 0 and 4
       or (
           v_receipt_status = 1
           and v_receipt_error <> 0
       )
       or (
           v_receipt_status = 2
           and v_receipt_error not in (1, 2)
       )
       or (
           v_receipt_status = 3
           and v_receipt_error not in (3, 4)
       ) then
        raise exception using
            errcode = '55000',
            message = 'command acknowledgement returned an invalid receipt';
    end if;

    v_payload := pg_catalog.replace(
        v_ack_receipt::text,
        ' ',
        ''
    );

    if pg_catalog.octet_length(v_payload) > 80 then
        raise exception using
            errcode = '54000',
            message = 'command receipt payload exceeds modem limit';
    end if;

    select
        (
            select s.decrypted_secret
            from vault.decrypted_secrets as s
            where s.name = 'nb_iot_emqx_publish_url'
        ),
        (
            select s.decrypted_secret
            from vault.decrypted_secrets as s
            where s.name = 'nb_iot_emqx_publish_key'
        ),
        (
            select s.decrypted_secret
            from vault.decrypted_secrets as s
            where s.name = 'nb_iot_emqx_publish_secret'
        )
    into
        v_emqx_publish_url,
        v_emqx_publish_key,
        v_emqx_publish_secret;

    if nullif(v_emqx_publish_url, '') is null
       or nullif(v_emqx_publish_key, '') is null
       or nullif(v_emqx_publish_secret, '') is null then
        raise exception using
            errcode = '55000',
            message = 'command publish destination is not configured';
    end if;

    select net.http_post(
        url := v_emqx_publish_url,
        headers := pg_catalog.jsonb_build_object(
            'Content-Type',
            'application/json',
            'Authorization', 'Basic ' || pg_catalog.replace(
                pg_catalog.encode(
                    pg_catalog.convert_to(
                        v_emqx_publish_key || ':' || v_emqx_publish_secret,
                        'UTF8'
                    ),
                    'base64'
                ),
                E'\n',
                ''
            )
        ),
        body := pg_catalog.jsonb_build_object(
            'topic',
            'devices/' || p_imei || '/cmd/ack/receipt',
            'qos', 1,
            'retain', false,
            'payload',
            v_payload
        ),
        timeout_milliseconds := 5000
    )
    into v_http_request_id;

    if v_http_request_id is null then
        raise exception using
            errcode = '55000',
            message = 'command publish request was not queued';
    end if;

    return v_http_request_id;
end;
$function$;

revoke all on function public.publish_device_command_response(text,bigint,bigint,text)
    from public, anon, authenticated, service_role;
grant execute on function public.publish_device_command_response(text,bigint,bigint,text)
    to anon, service_role;

revoke all on function public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)
    from public, anon, authenticated, service_role;
grant execute on function public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)
    to anon, service_role;

revoke all on function public.claim_device_command(text,bigint,bigint,integer)
    from public, anon, authenticated;
grant execute on function public.claim_device_command(text,bigint,bigint,integer)
    to service_role;

revoke all on function public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)
    from public, anon, authenticated;
grant execute on function public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)
    to service_role;

do $mark_command_gateway_provenance$
declare
    v_owner_oid oid;
    v_wrapper_oid oid;
begin
    v_wrapper_oid :=
        'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure;

    select p.proowner
    into strict v_owner_oid
    from pg_catalog.pg_proc as p
    where p.oid = v_wrapper_oid;

    execute pg_catalog.format(
        'comment on function public.publish_device_command_response'
            || '(text,bigint,bigint,text) is %L',
        'Claims one command and queues its compact MQTT response. '
            || 'Returned bigint is a queued request ID only, '
            || 'not HTTP or MQTT delivery success;definition_md5='
            || pg_catalog.md5(
                pg_catalog.pg_get_functiondef(v_wrapper_oid)
            )
            || ';owner_oid='
            || v_owner_oid::text
    );

    v_wrapper_oid :=
        'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure;

    select p.proowner
    into strict v_owner_oid
    from pg_catalog.pg_proc as p
    where p.oid = v_wrapper_oid;

    execute pg_catalog.format(
        'comment on function public.publish_device_command_ack_receipt'
            || '(text,bigint,smallint,smallint,smallint,bigint,boolean,text) '
            || 'is %L',
        'Stores one ACK and queues its compact MQTT receipt. '
            || 'Returned bigint is a queued request ID only, '
            || 'not HTTP or MQTT delivery success;definition_md5='
            || pg_catalog.md5(
                pg_catalog.pg_get_functiondef(v_wrapper_oid)
            )
            || ';owner_oid='
            || v_owner_oid::text
    );
end;
$mark_command_gateway_provenance$;

drop trigger trg_assign_device_command on public.sensorvalue;

commit;
