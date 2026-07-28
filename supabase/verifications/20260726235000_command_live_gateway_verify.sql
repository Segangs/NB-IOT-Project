begin;

set local statement_timeout = '30s';

do $verify$
declare
    v_ack_queue_count integer;
    v_ack_request_id bigint;
    v_assign_md5 text;
    v_claim_receipt jsonb;
    v_config_request_secret text;
    v_emqx_publish_key text;
    v_emqx_publish_secret text;
    v_emqx_publish_url text;
    v_expected_ack_payload text;
    v_expected_authorization text;
    v_expected_response_payload text;
    v_gateway_request_secret text;
    v_legacy_trigger_count integer;
    v_probe_imei text;
    v_probe_request_id bigint := 4294967295;
    v_response_queue_count integer;
    v_response_request_id bigint;
    v_sync_trigger_count integer;
    v_wrapper_count integer;
begin
    select s.decrypted_secret
    into strict v_config_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_config_request_secret'
      and nullif(s.decrypted_secret, '') is not null;

    select s.decrypted_secret
    into strict v_gateway_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_command_gateway_secret'
      and nullif(s.decrypted_secret, '') is not null;

    if v_gateway_request_secret is distinct from
       v_config_request_secret then
        raise exception 'command gateway Vault alias equality mismatch';
    end if;

    if (
        select count(*)
        from vault.decrypted_secrets as s
        join vault.secrets as d
          on s.id = d.id
        where s.name = 'nb_iot_command_gateway_secret'
          and d.name = 'nb_iot_command_gateway_secret'
          and d.description =
              'NB-IOT command gateway migration 20260726235000;secret_id='
                  || s.id::text
          and s.decrypted_secret is not distinct from
              v_config_request_secret
    ) <> 1 then
        raise exception 'command gateway Vault alias provenance mismatch';
    end if;

    select count(*)
    into v_wrapper_count
    from pg_catalog.pg_proc as p
    join pg_catalog.pg_namespace as n
      on n.oid = p.pronamespace
    where n.nspname = 'public'
      and p.proname in (
          'publish_device_command_response',
          'publish_device_command_ack_receipt'
      );

    if v_wrapper_count <> 2
       or to_regprocedure(
           'public.publish_device_command_response(text,bigint,bigint,text)'
       ) is null
       or to_regprocedure(
           'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'
       ) is null then
        raise exception 'command gateway wrapper cardinality mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        join pg_catalog.pg_language as l
          on l.oid = p.prolang
        where p.oid in (
            'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure,
            'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure
        )
          and (
              p.prokind <> 'f'
              or p.prorettype <> 'pg_catalog.int8'::regtype
              or l.lanname <> 'plpgsql'
              or not p.prosecdef
              or not exists (
                  select 1
                  from pg_catalog.unnest(
                      coalesce(p.proconfig, array[]::text[])
                  ) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
          )
    ) then
        raise exception 'command gateway wrapper definition mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure,
            'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure
        )
          and pg_catalog.obj_description(p.oid, 'pg_proc')
              is distinct from (
                  case p.oid
                      when 'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure
                          then 'Claims one command and queues its compact MQTT response. Returned bigint is a queued request ID only, not HTTP or MQTT delivery success'
                      else 'Stores one ACK and queues its compact MQTT receipt. Returned bigint is a queued request ID only, not HTTP or MQTT delivery success'
                  end
                  || ';definition_md5='
                  || pg_catalog.md5(
                      pg_catalog.pg_get_functiondef(p.oid)
                  )
                  || ';owner_oid='
                  || p.proowner::text
              )
    ) then
        raise exception 'command gateway wrapper provenance mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure,
            'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure
        )
          and (
              not has_function_privilege('anon', p.oid, 'EXECUTE')
              or has_function_privilege(
                  'authenticated',
                  p.oid,
                  'EXECUTE'
              )
              or not has_function_privilege(
                  'service_role',
                  p.oid,
                  'EXECUTE'
              )
          )
    ) then
        raise exception 'command gateway wrapper role ACL mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        cross join lateral pg_catalog.aclexplode(
            coalesce(
                p.proacl,
                pg_catalog.acldefault('f', p.proowner)
            )
        ) as acl
        where p.oid in (
            'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure,
            'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure
        )
          and acl.privilege_type = 'EXECUTE'
          and (
              acl.grantee not in (
                  p.proowner,
                  'anon'::regrole,
                  'service_role'::regrole
              )
              or (
                  acl.grantee in (
                      'anon'::regrole,
                      'service_role'::regrole
                  )
                  and acl.is_grantable
              )
          )
    ) then
        raise exception 'command gateway wrapper has an out-of-allowlist direct ACL';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure,
            'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure
        )
          and (
              not exists (
                  select 1
                  from pg_catalog.aclexplode(
                      coalesce(
                          p.proacl,
                          pg_catalog.acldefault('f', p.proowner)
                      )
                  ) as acl
                  where acl.grantee = 'anon'::regrole
                    and acl.privilege_type = 'EXECUTE'
                    and not acl.is_grantable
              )
              or not exists (
                  select 1
                  from pg_catalog.aclexplode(
                      coalesce(
                          p.proacl,
                          pg_catalog.acldefault('f', p.proowner)
                      )
                  ) as acl
                  where acl.grantee = 'service_role'::regrole
                    and acl.privilege_type = 'EXECUTE'
                    and not acl.is_grantable
              )
          )
    ) then
        raise exception 'command gateway wrapper direct ACL is incomplete';
    end if;

    if to_regprocedure(
        'public.claim_device_command(text,bigint,bigint,integer)'
    ) is null
       or to_regprocedure(
           'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'
       ) is null then
        raise exception 'internal command RPC is missing';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.claim_device_command(text,bigint,bigint,integer)'::regprocedure,
            'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'::regprocedure
        )
          and (
              not p.prosecdef
              or has_function_privilege('anon', p.oid, 'EXECUTE')
              or has_function_privilege(
                  'authenticated',
                  p.oid,
                  'EXECUTE'
              )
              or not has_function_privilege(
                  'service_role',
                  p.oid,
                  'EXECUTE'
              )
          )
    ) then
        raise exception 'internal command RPC role ACL mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        cross join lateral pg_catalog.aclexplode(
            coalesce(
                p.proacl,
                pg_catalog.acldefault('f', p.proowner)
            )
        ) as acl
        where p.oid in (
            'public.claim_device_command(text,bigint,bigint,integer)'::regprocedure,
            'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'::regprocedure
        )
          and acl.privilege_type = 'EXECUTE'
          and acl.grantee not in (
              p.proowner,
              'service_role'::regrole
          )
    ) then
        raise exception 'internal command RPC has an out-of-allowlist direct ACL';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_assign_device_command'
          and not t.tgisinternal
    ) then
        raise exception 'legacy trigger trg_assign_device_command remains active';
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

    if v_legacy_trigger_count <> 0 then
        raise exception 'exact legacy trigger remains active';
    end if;

    if to_regprocedure('public.assign_device_command()') is null then
        raise exception 'legacy rollback function assign_device_command() is missing';
    end if;

    select pg_catalog.md5(
        pg_catalog.pg_get_functiondef(
            'public.assign_device_command()'::regprocedure
        )
    )
    into v_assign_md5;

    if v_assign_md5 <> 'de97b37ad246c5e509ae06f821a10338' then
        raise exception 'legacy assign_device_command() changed unexpectedly';
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
        raise exception 'trg_sync_device_command_state definition mismatch';
    end if;

    select pg_catalog.lpad(candidate.value::text, 20, '0')
    into v_probe_imei
    from pg_catalog.generate_series(0, 1000) as candidate(value)
    where not exists (
        select 1
        from public.device as d
        where d."deviceIMEI" =
            pg_catalog.lpad(candidate.value::text, 20, '0')
    )
    order by candidate.value
    limit 1;

    if v_probe_imei is null then
        raise exception 'no unused device identity is available for gateway verification';
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
        raise exception 'command publish destination is not configured';
    end if;

    v_expected_authorization := 'Basic ' || pg_catalog.replace(
        pg_catalog.encode(
            pg_catalog.convert_to(
                v_emqx_publish_key || ':' || v_emqx_publish_secret,
                'UTF8'
            ),
            'base64'
        ),
        E'\n',
        ''
    );
    v_expected_response_payload := pg_catalog.replace(
        pg_catalog.jsonb_build_array(
            v_probe_request_id,
            0,
            0,
            0,
            0
        )::text,
        ' ',
        ''
    );
    v_expected_ack_payload := pg_catalog.replace(
        pg_catalog.jsonb_build_array(
            4294967295,
            1,
            1,
            2,
            2
        )::text,
        ' ',
        ''
    );

    begin
        select public.claim_device_command(
            v_probe_imei,
            v_probe_request_id,
            4294967295,
            60
        )
        into v_claim_receipt;
    exception
        when sqlstate '55000' then
            raise exception 'no-command claim remains blocked by legacy gate';
    end;

    if v_claim_receipt is distinct from
       pg_catalog.jsonb_build_array(
           v_probe_request_id,
           0,
           0,
           0,
           0
       ) then
        raise exception 'no-command claim returned an unexpected receipt';
    end if;

    begin
        select public.publish_device_command_response(
            v_probe_imei,
            v_probe_request_id,
            4294967295,
            v_gateway_request_secret
        )
        into v_response_request_id;
    exception
        when sqlstate '55000' then
            raise exception 'command response wrapper execution failed';
    end;

    select public.publish_device_command_ack_receipt(
        v_probe_imei,
        4294967295::bigint,
        1::smallint,
        1::smallint,
        0::smallint,
        0::bigint,
        false,
        v_gateway_request_secret
    )
    into v_ack_request_id;

    if v_response_request_id is null
       or v_ack_request_id is null
       or v_response_request_id = v_ack_request_id then
        raise exception 'command gateway returned an invalid queued request ID';
    end if;

    if pg_catalog.octet_length(v_expected_response_payload) > 80
       or pg_catalog.octet_length(v_expected_response_payload) <= 0
       or pg_catalog.octet_length(v_expected_ack_payload) > 80
       or pg_catalog.octet_length(v_expected_ack_payload) <= 0 then
        raise exception 'command gateway verification payload limit mismatch';
    end if;

    select count(*)
    into v_response_queue_count
    from net.http_request_queue as q
    where q.id = v_response_request_id
      and q.method = 'POST'
      and q.url = v_emqx_publish_url
      and q.headers ->> 'Content-Type' = 'application/json'
      and q.headers ->> 'Authorization' = v_expected_authorization
      and q.headers = pg_catalog.jsonb_build_object(
          'Content-Type',
          'application/json',
          'Authorization',
          v_expected_authorization
      )
      and q.timeout_milliseconds = 5000
      and pg_catalog.convert_from(q.body, 'UTF8')::jsonb =
          pg_catalog.jsonb_build_object(
              'topic',
              'devices/' || v_probe_imei || '/cmd/response',
              'qos', 1,
              'retain', false,
              'payload', v_expected_response_payload
          );

    if v_response_queue_count <> 1 then
        raise exception 'command response queue row mismatch';
    end if;

    select count(*)
    into v_ack_queue_count
    from net.http_request_queue as q
    where q.id = v_ack_request_id
      and q.method = 'POST'
      and q.url = v_emqx_publish_url
      and q.headers ->> 'Content-Type' = 'application/json'
      and q.headers ->> 'Authorization' = v_expected_authorization
      and q.headers = pg_catalog.jsonb_build_object(
          'Content-Type',
          'application/json',
          'Authorization',
          v_expected_authorization
      )
      and q.timeout_milliseconds = 5000
      and pg_catalog.convert_from(q.body, 'UTF8')::jsonb =
          pg_catalog.jsonb_build_object(
              'topic',
              'devices/' || v_probe_imei || '/cmd/ack/receipt',
              'qos', 1,
              'retain', false,
              'payload', v_expected_ack_payload
          );

    if v_ack_queue_count <> 1 then
        raise exception 'command ACK receipt queue row mismatch';
    end if;
end;
$verify$;

select
    'command live gateway verification passed'::text as status;

rollback;
