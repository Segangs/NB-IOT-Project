begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.sensorvalue in share row exclusive mode;
lock table public."deviceCmds" in share row exclusive mode;

do $rollback_guard$
declare
    v_assign_md5 text;
    v_config_request_secret text;
    v_gateway_secret_id uuid;
    v_legacy_trigger_count integer;
    v_sync_trigger_count integer;
    v_wrapper_count integer;
begin
    if exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_assign_device_command'
          and not t.tgisinternal
    ) then
        raise exception using
            errcode = '42710',
            message = 'legacy command trigger already exists';
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
        raise exception using
            errcode = '42710',
            message = 'exact legacy command trigger already exists';
    end if;

    if to_regprocedure('public.assign_device_command()') is null then
        raise exception using
            errcode = '55000',
            message = 'legacy rollback function is missing';
    end if;

    select pg_catalog.md5(
        pg_catalog.pg_get_functiondef(
            'public.assign_device_command()'::regprocedure
        )
    )
    into v_assign_md5;

    if v_assign_md5 <> 'de97b37ad246c5e509ae06f821a10338' then
        raise exception using
            errcode = '55000',
            message = 'legacy rollback function definition drift';
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
        raise exception using
            errcode = '55000',
            message = 'command gateway wrapper cardinality drift';
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
        raise exception using
            errcode = '55000',
            message = 'command gateway wrapper definition drift';
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
        raise exception using
            errcode = '55000',
            message = 'command gateway wrapper provenance drift; manual rollback decision required';
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
        raise exception using
            errcode = '55000',
            message = 'command gateway wrapper role ACL drift';
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
        raise exception using
            errcode = '55000',
            message = 'command gateway wrapper direct ACL provenance drift';
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
        raise exception using
            errcode = '55000',
            message = 'command gateway wrapper direct ACL is incomplete';
    end if;

    select s.id
    into strict v_gateway_secret_id
    from vault.secrets as s
    where s.name = 'nb_iot_command_gateway_secret';

    select s.decrypted_secret
    into strict v_config_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_config_request_secret'
      and nullif(s.decrypted_secret, '') is not null;

    if (
        select count(*)
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
              v_config_request_secret
    ) <> 1 then
        raise exception using
            errcode = '55000',
            message = 'command gateway secret alias provenance drift; manual rollback decision required';
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
end;
$rollback_guard$;

create trigger trg_assign_device_command
before insert on public.sensorvalue
for each row
execute function public.assign_device_command();

do $verify_legacy_trigger_restore$
declare
    v_legacy_trigger_count integer;
    v_sync_trigger_count integer;
begin
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
            message = 'legacy command trigger restoration failed';
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
            message = 'command state sync trigger changed during rollback';
    end if;
end;
$verify_legacy_trigger_restore$;

drop function public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text);
drop function public.publish_device_command_response(text,bigint,bigint,text);

do $remove_command_gateway_secret$
declare
    v_config_request_secret text;
    v_deleted_count integer;
    v_gateway_secret_id uuid;
begin
    select s.id
    into strict v_gateway_secret_id
    from vault.secrets as s
    where s.name = 'nb_iot_command_gateway_secret';

    select s.decrypted_secret
    into strict v_config_request_secret
    from vault.decrypted_secrets as s
    where s.name = 'nb_iot_config_request_secret'
      and nullif(s.decrypted_secret, '') is not null;

    delete from vault.secrets as target
    where target.id = v_gateway_secret_id
      and target.name = 'nb_iot_command_gateway_secret'
      and target.description =
          'NB-IOT command gateway migration 20260726235000;secret_id='
              || v_gateway_secret_id::text
      and exists (
          select 1
          from vault.decrypted_secrets as alias_secret
          where alias_secret.id = v_gateway_secret_id
            and alias_secret.name = 'nb_iot_command_gateway_secret'
            and alias_secret.decrypted_secret is not distinct from
                v_config_request_secret
      );

    get diagnostics v_deleted_count = row_count;

    if v_deleted_count <> 1 then
        raise exception using
            errcode = '55000',
            message = 'command gateway secret alias removal failed';
    end if;
end;
$remove_command_gateway_secret$;

commit;
