begin read only;

set local statement_timeout = '30s';

do $precheck$
begin
    if current_setting('server_version_num')::integer < 150000 then
        raise exception 'PostgreSQL 15 or newer is required';
    end if;

    if to_regclass('public.device') is null
       or to_regclass('public.users') is null then
        raise exception 'required public.device or public.users table is missing';
    end if;

    if not exists (
        select 1
        from pg_catalog.pg_roles as r
        where r.rolname = 'service_role'
    ) then
        raise exception 'required service_role is missing';
    end if;

    if to_regclass('public.alert_contact') is not null
       or to_regclass('public.message_policy') is not null
       or to_regclass('public.msg_send') is not null
       or to_regclass('public.message_link_token') is not null
       or to_regclass(
           'public.alert_contact_alert_contact_id_seq'
       ) is not null
       or to_regclass(
           'public.message_policy_message_policy_id_seq'
       ) is not null
       or to_regclass('public.msg_send_msg_send_id_seq') is not null
       or to_regclass(
           'public.message_link_token_message_link_token_id_seq'
       ) is not null
       or to_regclass('public.idx_msg_send_claim') is not null
       or to_regclass('public.idx_msg_send_lease') is not null
       or to_regclass(
           'public.uq_msg_send_provider_request'
       ) is not null
       or to_regclass(
           'public.uq_msg_send_fallback_provider_request'
       ) is not null
       or to_regclass('public.uq_msg_send_provider_result') is not null
       or to_regclass(
           'public.uq_msg_send_fallback_provider_result'
       ) is not null
       or to_regclass(
           'public.idx_message_link_token_expiry'
       ) is not null then
        raise exception 'one or more target message tables already exist';
    end if;

    if to_regprocedure(
           'public.claim_msg_send(text,integer,integer)'
       ) is not null
       or to_regprocedure(
           'public.claim_exact_one_shot_msg_send(bigint,text,integer)'
       ) is not null
       or to_regprocedure(
           'public.mark_msg_send_submission_started(bigint,text,uuid,text)'
       ) is not null
       or to_regprocedure(
           'public.mark_msg_send_submission_waiting_result(bigint,text,uuid,text,text,timestamptz)'
       ) is not null
       or to_regprocedure(
           'public.complete_msg_send_claim(bigint,text,uuid,text,timestamptz,text)'
       ) is not null
       or to_regprocedure(
           'public.recover_msg_send_leases(integer,integer,integer,integer,integer)'
       ) is not null
       or to_regprocedure(
           'public.record_msg_send_push_result(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,text,integer,integer,integer,integer)'
       ) is not null then
        raise exception 'one or more target message functions already exist';
    end if;

    if not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'device'
          and c.column_name = 'deviceId'
          and c.data_type = 'integer'
          and c.is_nullable = 'NO'
    ) then
        raise exception 'public.device deviceId contract drift';
    end if;

    if not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'users'
          and c.column_name = 'userId'
          and c.data_type = 'integer'
          and c.is_nullable = 'NO'
    ) then
        raise exception 'public.users userId contract drift';
    end if;
end;
$precheck$;

rollback;
