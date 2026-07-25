begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public."deviceCmds" in share row exclusive mode;

drop trigger trg_sync_device_command_state on public."deviceCmds";
drop function public.sync_device_command_state();
drop function public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean);
drop function public.claim_device_command(text,bigint,bigint,integer);
drop function public.device_command_receipt(bigint,smallint,smallint,smallint,smallint);
drop table public.device_command_state;

commit;
