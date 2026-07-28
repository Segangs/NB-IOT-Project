begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_definition text;
begin
    select pg_catalog.pg_get_constraintdef(c.oid)
    into v_definition
    from pg_catalog.pg_constraint as c
    where c.conrelid = 'public.msg_send'::regclass
      and c.conname = 'msg_send_incident_template_stage_unique'
      and c.contype = 'u';
    if v_definition is null
       or pg_catalog.regexp_replace(
              pg_catalog.lower(v_definition),
              '\s+',
              '',
              'g'
          ) <>
          'unique(source_device_id,incident_id,template_code,template_stage,alert_contact_id,source_event_sequence)'
    then
        raise exception 'repeat msg_send unique contract drift';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_constraint as c
        where c.conrelid = 'public.msg_send'::regclass
          and c.conname = 'msg_send_dedupe_key_unique'
          and c.contype = 'u'
          and pg_catalog.regexp_replace(
                  pg_catalog.lower(
                      pg_catalog.pg_get_constraintdef(c.oid)
                  ),
                  '\s+',
                  '',
                  'g'
              ) = 'unique(dedupe_key)'
    ) then
        raise exception 'msg_send_dedupe_key_unique drift';
    end if;

    if exists (
        select 1
        from public.msg_send
        group by
            source_device_id,
            incident_id,
            template_code,
            template_stage,
            alert_contact_id,
            source_event_sequence
        having pg_catalog.count(*) > 1
    ) then
        raise exception 'repeat msg_send source event duplicate';
    end if;

    if not (
        select c.relrowsecurity
        from pg_catalog.pg_class as c
        where c.oid = 'public.msg_send'::regclass
    ) then
        raise exception 'msg_send relrowsecurity drift';
    end if;

    if pg_catalog.has_table_privilege(
           'anon',
           'public.msg_send',
           'SELECT,INSERT,UPDATE,DELETE'
       )
       or pg_catalog.has_table_privilege(
           'authenticated',
           'public.msg_send',
           'SELECT,INSERT,UPDATE,DELETE'
       )
       or not pg_catalog.has_table_privilege(
           'service_role',
           'public.msg_send',
           'SELECT,INSERT,UPDATE,DELETE'
       ) then
        raise exception 'msg_send ACL drift';
    end if;
end;
$verify$;

select
    (
        select pg_catalog.count(*)
        from public.msg_send
        where incident_kind = 'temperature'
    ) as temperature_msg_send_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where notification_ordinal > 1
    ) as repeat_outbox_count;

rollback;
