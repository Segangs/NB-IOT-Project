begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

drop index public.idx_msg_send_power_incident_order;

create or replace function public.claim_msg_send(
    p_lock_owner text,
    p_batch_size integer,
    p_lease_seconds integer
)
returns setof public.msg_send
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_now timestamptz;
begin
    if p_lock_owner is null
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner))
          not between 1 and 128 then
        raise exception 'invalid lock owner'
            using errcode = '22023';
    end if;
    if p_batch_size is null or p_batch_size not between 1 and 50 then
        raise exception 'batch size must be between 1 and 50'
            using errcode = '22023';
    end if;
    if p_lease_seconds is null
       or p_lease_seconds not between 15 and 300 then
        raise exception 'lease seconds must be between 15 and 300'
            using errcode = '22023';
    end if;

    v_now := pg_catalog.clock_timestamp();

    return query
    with candidates as materialized (
        select m.msg_send_id
        from public.msg_send as m
        where m.status in ('pending', 'retry_wait')
          and m.available_at <= v_now
          and m.expires_at > v_now
          and m.attempt_count < m.max_attempts
        order by
            m.priority desc,
            m.available_at,
            m.msg_send_id
        limit p_batch_size
        for update skip locked
    ),
    claimed as (
        update public.msg_send as m
        set
            status = 'processing',
            lock_owner = p_lock_owner,
            lease_token = pg_catalog.gen_random_uuid(),
            lease_generation = m.lease_generation + 1,
            lease_expires_at =
                v_now
                + pg_catalog.make_interval(secs => p_lease_seconds),
            attempt_count = m.attempt_count + 1,
            last_attempt_at = v_now,
            updated_at = v_now
        from candidates as c
        where m.msg_send_id = c.msg_send_id
        returning m.*
    )
    select c.*
    from claimed as c
    order by
        c.priority desc,
        c.available_at,
        c.msg_send_id;
end;
$function$;

revoke all on function public.claim_msg_send(text,integer,integer)
    from public, anon, authenticated;
grant execute on function public.claim_msg_send(text,integer,integer)
    to service_role;

commit;
