begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_count integer;
    v_definition text;
begin
    if to_regclass('public.msg_send') is null
       or to_regprocedure(
           'public.claim_msg_send(text,integer,integer)'
       ) is null then
        raise exception 'message queue claim contract is missing';
    end if;

    if to_regclass(
           'public.idx_msg_send_power_incident_order'
       ) is not null then
        raise exception 'message queue ordering index already exists';
    end if;

    select pg_catalog.count(*)::integer
    into v_count
    from information_schema.columns as c
    where c.table_schema = 'public'
      and c.table_name = 'msg_send'
      and (
          (c.column_name = 'source_device_id'
           and c.data_type = 'integer')
          or (c.column_name = 'source_event_origin'
              and c.data_type = 'text')
          or (c.column_name = 'source_event_sequence'
              and c.data_type = 'bigint')
          or (c.column_name = 'incident_id'
              and c.data_type = 'bigint')
          or (c.column_name = 'status'
              and c.data_type = 'text')
          or (c.column_name = 'priority'
              and c.data_type = 'smallint')
          or (c.column_name = 'available_at'
              and c.data_type = 'timestamp with time zone')
      );

    if v_count <> 7 then
        raise exception 'required msg_send column contract drift';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid =
              'public.claim_msg_send(text,integer,integer)'::regprocedure
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
        raise exception 'message queue claim hardening drift';
    end if;

    if not has_function_privilege(
           'service_role',
           'public.claim_msg_send(text,integer,integer)',
           'EXECUTE'
       )
       or has_function_privilege(
           'anon',
           'public.claim_msg_send(text,integer,integer)',
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           'public.claim_msg_send(text,integer,integer)',
           'EXECUTE'
       ) then
        raise exception 'message queue claim privilege drift';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.claim_msg_send(text,integer,integer)'::regprocedure
    )
    into strict v_definition;

    if position('for update skip locked' in pg_catalog.lower(v_definition))
           = 0
       or position('lease_generation = m.lease_generation + 1'
                   in pg_catalog.lower(v_definition)) = 0 then
        raise exception 'message queue atomic claim contract drift';
    end if;
end;
$precheck$;

select
    pg_catalog.count(*) filter (
        where status in (
            'pending',
            'processing',
            'accepted',
            'waiting_result',
            'retry_wait'
        )
    ) as active_message_count,
    pg_catalog.count(*) filter (
        where source_event_origin = 'device_power_event'
    ) as power_message_count
from public.msg_send;

rollback;
