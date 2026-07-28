begin read only;

set local statement_timeout = '30s';

do $precheck$
declare
    v_definition text;
begin
    if to_regclass('public.msg_send') is null
       or to_regclass('public.temperature_alert_outbox') is null
       or not exists (
           select 1
           from information_schema.columns
           where table_schema = 'public'
             and table_name = 'temperature_alert_outbox'
             and column_name = 'notification_ordinal'
             and data_type = 'smallint'
       ) then
        raise exception 'temperature repeat msg_send base is missing';
    end if;

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
          'unique(source_device_id,incident_id,template_code,template_stage,alert_contact_id)'
    then
        raise exception
            'msg_send_incident_template_stage_unique drift';
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
        raise exception 'msg_send dedupe unique drift';
    end if;

    if exists (
        select 1
        from public.msg_send
        where status = 'processing'
    ) then
        raise exception 'processing msg_send must be zero';
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
        raise exception 'source event msg_send cardinality drift';
    end if;
end;
$precheck$;

select
    (
        select pg_catalog.count(*)
        from public.msg_send
        where status = 'processing'
    ) as processing_msg_send_count,
    (
        select pg_catalog.count(*)
        from public.temperature_alert_outbox
        where notification_ordinal > 1
    ) as repeat_outbox_count;

rollback;
