begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

drop function public.record_msg_send_push_result(
    text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,
    text,integer,integer,integer,integer
);
drop function public.recover_msg_send_leases(
    integer,integer,integer,integer,integer
);
drop function public.complete_msg_send_claim(
    bigint,text,uuid,text,timestamptz,text
);
drop function public.mark_msg_send_submission_waiting_result(
    bigint,text,uuid,text,text,timestamptz
);
drop function public.mark_msg_send_submission_started(
    bigint,text,uuid,text
);
drop function public.claim_exact_one_shot_msg_send(
    bigint,text,integer
);
drop function public.claim_msg_send(text,integer,integer);
drop table public.message_link_token;
drop table public.msg_send;
drop table public.message_policy;
drop table public.alert_contact;

commit;
