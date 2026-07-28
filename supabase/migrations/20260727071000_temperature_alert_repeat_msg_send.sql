begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public.msg_send in share row exclusive mode;

alter table public.msg_send
    drop constraint msg_send_incident_template_stage_unique,
    add constraint msg_send_incident_template_stage_unique
        unique (
            source_device_id,
            incident_id,
            template_code,
            template_stage,
            alert_contact_id,
            source_event_sequence
        );

commit;
