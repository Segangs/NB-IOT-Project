begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

drop function public.enqueue_temp_alert_msg_send(
    bigint,bigint,integer,integer,text,bigint,bigint,text,jsonb,text,
    timestamptz,smallint
);
drop function public.finalize_msg_send_provider_acceptance(
    bigint,text,uuid,text,text,timestamptz,numeric
);

commit;
