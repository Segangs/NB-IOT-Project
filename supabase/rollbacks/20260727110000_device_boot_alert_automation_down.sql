begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

drop trigger if exists trg_capture_device_boot_alert
    on public.device_boot_logs;
drop function if exists public.capture_device_boot_alert();

commit;
