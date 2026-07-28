begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
begin
    perform interval '20 minutes';

    if not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'temperature_alert_preference'
          and column_name = 'user_sensor_pk'
          and data_type = 'integer'
          and is_nullable = 'NO'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'temperature_alert_preference'
          and column_name = 'max_notifications'
          and data_type = 'smallint'
          and is_nullable = 'NO'
          and column_default = '3'
    ) or not exists (
        select 1
        from information_schema.columns
        where table_schema = 'public'
          and table_name = 'temperature_alert_preference'
          and column_name in ('created_at', 'updated_at')
          and data_type = 'timestamp with time zone'
        group by table_schema, table_name
        having pg_catalog.count(*) = 2
    ) then
        raise exception 'temperature_alert_preference column drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
              'public.temperature_alert_preference'::regclass
          and c.conname =
              'temperature_alert_preference_max_notifications_check'
          and pg_catalog.pg_get_constraintdef(c.oid)
              ilike
              '%max_notifications >= 1%max_notifications <= 3%'
    ) then
        raise exception
            'temperature_alert_preference max notification check drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
              'public.temperature_alert_preference'::regclass
          and c.contype = 'p'
          and pg_catalog.pg_get_constraintdef(c.oid)
              ilike '%user_sensor_pk%'
    ) or not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid =
              'public.temperature_alert_preference'::regclass
          and c.contype = 'f'
          and c.confrelid = 'public."USER_SENSOR"'::regclass
          and c.confdeltype = 'c'
    ) then
        raise exception
            'temperature_alert_preference key contract drift';
    end if;

    if not (
        select c.relrowsecurity
        from pg_catalog.pg_class as c
        where c.oid =
              'public.temperature_alert_preference'::regclass
    ) then
        raise exception 'temperature_alert_preference relrowsecurity drift';
    end if;

    if pg_catalog.has_table_privilege(
           'anon',
           'public.temperature_alert_preference',
           'SELECT'
       )
       or pg_catalog.has_table_privilege(
           'authenticated',
           'public.temperature_alert_preference',
           'SELECT'
       )
       or not pg_catalog.has_table_privilege(
           'service_role',
           'public.temperature_alert_preference',
           'SELECT,INSERT,UPDATE,DELETE'
       ) then
        raise exception 'temperature_alert_preference ACL drift';
    end if;

    if to_regprocedure(
           'public.get_device_temperature_settings(integer,integer,integer)'
       ) is null
       or to_regprocedure(
           'public.update_device_temperature_settings(integer,integer,integer,jsonb)'
       ) is null then
        raise exception 'temperature settings RPC missing';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.get_device_temperature_settings(integer,integer,integer)'::regprocedure,
            'public.update_device_temperature_settings(integer,integer,integer,jsonb)'::regprocedure,
            'public.capture_temperature_alert_transition()'::regprocedure
        )
          and (
              not p.prosecdef
              or p.proowner <>
                 (
                     select r.oid
                     from pg_catalog.pg_roles as r
                     where r.rolname = 'postgres'
                 )
              or not exists (
                  select 1
                  from pg_catalog.unnest(p.proconfig) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
          )
    ) then
        raise exception 'temperature settings function hardening drift';
    end if;

    if pg_catalog.has_function_privilege(
           'anon',
           'public.get_device_temperature_settings(integer,integer,integer)',
           'EXECUTE'
       )
       or pg_catalog.has_function_privilege(
           'authenticated',
           'public.get_device_temperature_settings(integer,integer,integer)',
           'EXECUTE'
       )
       or not pg_catalog.has_function_privilege(
           'service_role',
           'public.get_device_temperature_settings(integer,integer,integer)',
           'EXECUTE'
       )
       or pg_catalog.has_function_privilege(
           'anon',
           'public.update_device_temperature_settings(integer,integer,integer,jsonb)',
           'EXECUTE'
       )
       or pg_catalog.has_function_privilege(
           'authenticated',
           'public.update_device_temperature_settings(integer,integer,integer,jsonb)',
           'EXECUTE'
       )
       or not pg_catalog.has_function_privilege(
           'service_role',
           'public.update_device_temperature_settings(integer,integer,integer,jsonb)',
           'EXECUTE'
       ) then
        raise exception 'temperature settings RPC ACL drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.get_device_temperature_settings(integer,integer,integer)'::regprocedure
    )
    into v_definition;
    if v_definition not ilike
           '%coalesce(p.max_notifications, 3)%'
       or v_definition not ilike
           '%c."sensorCtgyType" = ''TMP''%'
       or v_definition not ilike
           '%d."userId" = p_user_id%'
       or v_definition not ilike
           '%d."userWorkplaceId" = p_workplace_id%' then
        raise exception 'temperature settings read scope drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.update_device_temperature_settings(integer,integer,integer,jsonb)'::regprocedure
    )
    into v_definition;
    if v_definition not ilike '%pg_advisory_xact_lock%'
       or v_definition not ilike '%jsonb_array_elements%'
       or v_definition not ilike '%jsonb_object_keys%'
       or v_definition not ilike '%setTmpUpLimit%'
       or v_definition not ilike
           '%on conflict on constraint%'
       or v_definition not ilike
           '%temperature_alert_preference_pkey%' then
        raise exception 'temperature settings update RPC drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.capture_temperature_alert_transition()'::regprocedure
    )
    into v_definition;
    if v_definition not ilike '%v_max_notifications%'
       or v_definition not ilike
           '%coalesce(p.max_notifications, 3)%'
       or v_definition not ilike
           '%high_notification_count < v_max_notifications%'
       or v_definition not ilike '%interval ''20 minutes''%'
       or v_definition ilike
           '%high_notification_count < 3%' then
        raise exception 'temperature settings trigger drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_capture_temperature_alert_transition'
          and not t.tgisinternal
          and t.tgenabled = 'O'
          and t.tgfoid =
              'public.capture_temperature_alert_transition()'::regprocedure
    ) then
        raise exception 'temperature alert trigger drift';
    end if;

    if exists (
        select 1
        from public.temperature_alert_preference
        where max_notifications < 1
           or max_notifications > 3
    ) then
        raise exception 'temperature preference data drift';
    end if;
end;
$verify$;

select
    (
        select pg_catalog.count(*)
        from public.temperature_alert_preference
    ) as explicit_preference_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_state
        where is_high
    ) as active_high_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where status = 'processing'
    ) as processing_outbox_count;

rollback;
