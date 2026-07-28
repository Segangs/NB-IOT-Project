begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_assign_md5 text;
    v_legacy_trigger_count integer;
    v_required_secret_count integer;
    v_sync_trigger_count integer;
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if to_regclass('public.sensorvalue') is null
       or to_regclass('public."deviceCmds"') is null
       or to_regclass('public.device') is null
       or to_regclass('public.device_command_state') is null then
        raise exception 'command gateway prerequisite relation is missing';
    end if;

    if to_regprocedure(
        'public.claim_device_command(text,bigint,bigint,integer)'
    ) is null
       or to_regprocedure(
           'public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)'
       ) is null
       or to_regprocedure('public.sync_device_command_state()') is null then
        raise exception 'command gateway prerequisite RPC is missing';
    end if;

    if to_regclass('net.http_request_queue') is null
       or to_regprocedure(
        'net.http_post(text,jsonb,jsonb,jsonb,integer)'
    ) is null then
        raise exception 'pg_net HTTP publish boundary is missing';
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
        raise exception 'Vault boundary is missing';
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
        raise exception 'legacy trigger trg_assign_device_command definition drift';
    end if;

    select md5(
        pg_catalog.pg_get_functiondef(
            'public.assign_device_command()'::regprocedure
        )
    )
    into v_assign_md5;

    if v_assign_md5 <> 'de97b37ad246c5e509ae06f821a10338' then
        raise exception 'legacy assign_device_command() definition drift';
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
        raise exception 'trg_sync_device_command_state definition drift';
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
              or not exists (
                  select 1
                  from pg_catalog.unnest(
                      coalesce(p.proconfig, array[]::text[])
                  ) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
              or has_function_privilege('anon', p.oid, 'EXECUTE')
              or has_function_privilege('authenticated', p.oid, 'EXECUTE')
              or not has_function_privilege(
                  'service_role',
                  p.oid,
                  'EXECUTE'
              )
          )
    ) then
        raise exception 'internal command RPC security boundary drift';
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
          and acl.grantee = 0
          and acl.privilege_type = 'EXECUTE'
    ) then
        raise exception 'internal command RPC remains executable by PUBLIC';
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
        raise exception 'one or more command gateway target functions already exist';
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
        raise exception 'one or more command gateway prerequisite secrets are missing';
    end if;

    if exists (
        select 1
        from vault.decrypted_secrets as s
        where s.name = 'nb_iot_command_gateway_secret'
    ) then
        raise exception 'target Vault alias nb_iot_command_gateway_secret already exists';
    end if;

    if exists (
        select 1
        from public."deviceCmds"
        where status = 0
    ) then
        raise exception 'pending legacy command rows block gateway cutover';
    end if;
end;
$precheck$;

select
    'command live gateway precheck passed'::text as status;

rollback;
