begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
    v_compact_definition text;
    v_index_definition text;
begin
    if to_regprocedure(
           'public.claim_msg_send(text,integer,integer)'
       ) is null then
        raise exception 'message queue claim function is missing';
    end if;

    if to_regclass(
           'public.idx_msg_send_power_incident_order'
       ) is null then
        raise exception 'message queue ordering index is missing';
    end if;

    select pg_catalog.pg_get_functiondef(
        'public.claim_msg_send(text,integer,integer)'::regprocedure
    )
    into strict v_definition;

    v_compact_definition := pg_catalog.regexp_replace(
        pg_catalog.lower(v_definition),
        '[[:space:]]+',
        ' ',
        'g'
    );

    if position(
           'source_event_origin <> ''device_power_event'''
           in v_compact_definition
       ) = 0
       or position(
           'p.source_event_sequence < m.source_event_sequence'
           in v_compact_definition
       ) = 0
       or position(
           'for update skip locked'
           in v_compact_definition
       ) = 0 then
        raise exception 'message queue ordering function drift';
    end if;

    select pg_catalog.pg_get_indexdef(i.indexrelid)
    into strict v_index_definition
    from pg_catalog.pg_index as i
    where i.indexrelid =
          'public.idx_msg_send_power_incident_order'::regclass;

    if position(
           'source_device_id, incident_id, source_event_sequence, msg_send_id'
           in v_index_definition
       ) = 0
       or position(
           'source_event_origin = ''device_power_event''::text'
           in v_index_definition
       ) = 0 then
        raise exception 'message queue ordering index drift';
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
end;
$verify$;

select
    status,
    pg_catalog.count(*) as message_count
from public.msg_send
group by status
order by status;

rollback;
