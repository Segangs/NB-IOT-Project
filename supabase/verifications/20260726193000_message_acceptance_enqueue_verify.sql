begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_acceptance regprocedure;
    v_enqueue regprocedure;
begin
    v_acceptance := to_regprocedure(
        'public.finalize_msg_send_provider_acceptance(bigint,text,uuid,text,text,timestamptz,numeric)'
    );
    v_enqueue := to_regprocedure(
        'public.enqueue_temp_alert_msg_send(bigint,bigint,integer,integer,text,bigint,bigint,text,jsonb,text,timestamptz,smallint)'
    );
    if v_acceptance is null or v_enqueue is null then
        raise exception 'one or more message closeout functions are missing';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (v_acceptance, v_enqueue)
          and (
              not p.prosecdef
              or p.provolatile <> 'v'
              or not exists (
                  select 1
                  from unnest(
                      case
                          when p.proconfig is null then array[]::text[]
                          else p.proconfig
                      end
                  ) as cfg
                  where cfg ~ '^search_path=(|\"\")$'
              )
          )
    ) then
        raise exception 'message closeout function security drift';
    end if;

    if has_function_privilege(
           'anon',
           v_acceptance,
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           v_acceptance,
           'EXECUTE'
       )
       or not has_function_privilege(
           'service_role',
           v_acceptance,
           'EXECUTE'
       )
       or has_function_privilege(
           'anon',
           v_enqueue,
           'EXECUTE'
       )
       or has_function_privilege(
           'authenticated',
           v_enqueue,
           'EXECUTE'
       )
       or not has_function_privilege(
           'service_role',
           v_enqueue,
           'EXECUTE'
       ) then
        raise exception 'message closeout execute privilege drift';
    end if;

    if to_regclass('public.msg_send') is null
       or to_regclass('public.message_link_token') is null
       or not (
           select c.relrowsecurity
           from pg_catalog.pg_class as c
           where c.oid = 'public.msg_send'::regclass
       )
       or not (
           select c.relrowsecurity
           from pg_catalog.pg_class as c
           where c.oid = 'public.message_link_token'::regclass
       ) then
        raise exception 'message table baseline drift';
    end if;
end;
$verify$;

rollback;
