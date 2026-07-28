begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
begin
    if to_regprocedure(
           'public.capture_device_boot_alert()'
       ) is null then
        raise exception 'device boot alert function is missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.device_boot_logs'::regclass
          and t.tgname = 'trg_capture_device_boot_alert'
          and t.tgenabled = 'O'
          and not t.tgisinternal
          and t.tgfoid =
              'public.capture_device_boot_alert()'::regprocedure
    ) then
        raise exception 'device boot alert trigger drift';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid =
              'public.capture_device_boot_alert()'::regprocedure
          and (
              not p.prosecdef
              or p.provolatile <> 'v'
              or not exists (
                  select 1
                  from pg_catalog.unnest(p.proconfig) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
          )
    ) then
        raise exception 'device boot alert function hardening drift';
    end if;

    if has_function_privilege(
           'anon',
           'public.capture_device_boot_alert()',
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           'public.capture_device_boot_alert()',
           'EXECUTE'
       )
       or not has_function_privilege(
           'service_role',
           'public.capture_device_boot_alert()',
           'EXECUTE'
       ) then
        raise exception 'device boot alert function privilege drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.capture_device_boot_alert()'::regprocedure
    )
    into strict v_definition;

    if position('/rpc/b' in v_definition) = 0
       or position('interval ''5 minutes''' in v_definition) = 0
       or position(
           'bizp_2026071315003676625784727'
           in v_definition
       ) = 0 then
        raise exception 'device boot alert runtime contract drift';
    end if;
end;
$verify$;

select
    pg_catalog.count(*) filter (
        where source_event_type = 'device_boot'
    ) as device_boot_message_count,
    pg_catalog.count(*) filter (
        where status in (
            'pending',
            'processing',
            'accepted',
            'waiting_result',
            'retry_wait'
        )
    ) as active_message_count
from public.msg_send;

rollback;
