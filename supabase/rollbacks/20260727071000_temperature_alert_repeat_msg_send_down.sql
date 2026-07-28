begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

do $rollback_gate$
begin
    if exists (
        select 1
        from public.msg_send
        group by
            source_device_id,
            incident_id,
            template_code,
            template_stage,
            alert_contact_id
        having pg_catalog.count(*) > 1
    ) then
        raise exception
            'repeat msg_send data exists; explicit data decision required';
    end if;
end;
$rollback_gate$;

lock table public.msg_send in share row exclusive mode;

alter table public.msg_send
    drop constraint msg_send_incident_template_stage_unique,
    add constraint msg_send_incident_template_stage_unique
        unique (
            source_device_id,
            incident_id,
            template_code,
            template_stage,
            alert_contact_id
        );

commit;
