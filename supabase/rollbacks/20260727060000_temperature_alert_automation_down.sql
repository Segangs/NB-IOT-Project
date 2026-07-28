begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

drop trigger trg_capture_temperature_alert_transition
    on public.sensorvalue;
drop function public.capture_temperature_alert_transition();
drop function public.drain_temperature_alert_outbox(integer);
drop table public.temperature_alert_outbox;
drop table public.temperature_alert_state;
drop sequence public.temperature_alert_incident_id_seq;

commit;
