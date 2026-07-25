begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

lock table public."deviceCmds" in share row exclusive mode;

create table public.device_command_state (
    cmd_id integer primary key
        references public."deviceCmds" ("cmdId") on delete restrict,
    device_id integer not null
        references public.device ("deviceId") on delete restrict,
    opcode smallint not null,
    job_id bigint not null,
    state text not null default 'queued',
    request_id bigint,
    ttl_seconds integer not null default 86400,
    delivery_attempts integer not null default 0,
    lease_expires_at timestamp with time zone,
    created_at timestamp with time zone not null,
    expires_at timestamp with time zone not null,
    last_delivered_at timestamp with time zone,
    accepted_at timestamp with time zone,
    accepted_result smallint,
    accepted_error smallint,
    accepted_unix_seconds bigint,
    accepted_clock_valid boolean,
    final_at timestamp with time zone,
    final_result smallint,
    final_error smallint,
    final_unix_seconds bigint,
    final_clock_valid boolean,
    legacy_status smallint not null,
    updated_at timestamp with time zone not null default clock_timestamp(),
    constraint device_command_state_opcode_check
        check (opcode between 1 and 4),
    constraint device_command_state_job_id_check
        check (job_id between 1 and 4294967295),
    constraint device_command_state_state_check
        check (state in ('queued', 'delivered', 'accepted', 'executed', 'failed', 'expired')),
    constraint device_command_state_request_id_check
        check (request_id is null or request_id between 1 and 4294967295),
    constraint device_command_state_ttl_check
        check (ttl_seconds between 1 and 86400),
    constraint device_command_state_delivery_attempts_check
        check (delivery_attempts between 0 and 5),
    constraint device_command_state_legacy_status_check
        check (legacy_status between 0 and 1),
    constraint device_command_state_accepted_result_check
        check (accepted_result is null or accepted_result = 1),
    constraint device_command_state_accepted_error_check
        check (accepted_error is null or accepted_error = 0),
    constraint device_command_state_final_result_check
        check (final_result is null or final_result between 2 and 4),
    constraint device_command_state_final_error_check
        check (final_error is null or final_error between 0 and 5),
    constraint device_command_state_unix_seconds_check
        check (
            (accepted_unix_seconds is null or accepted_unix_seconds between 0 and 4294967295)
            and
            (final_unix_seconds is null or final_unix_seconds between 0 and 4294967295)
        ),
    constraint device_command_state_time_order_check
        check (expires_at > created_at),
    constraint device_command_state_terminal_check
        check (
            (
                state in ('queued', 'delivered', 'accepted')
                and final_result is null
                and final_error is null
                and final_at is null
            )
            or
            (
                state = 'executed'
                and final_result = 2
                and final_error = 0
                and final_at is not null
            )
            or
            (
                state = 'failed'
                and final_result = 3
                and final_error between 1 and 4
                and final_at is not null
            )
            or
            (
                state = 'expired'
                and final_result = 4
                and final_error = 5
                and final_at is not null
            )
        )
);

create index idx_device_command_state_claim
    on public.device_command_state (device_id, state, expires_at, cmd_id)
    where state in ('queued', 'delivered');

create unique index uq_device_command_state_active_request
    on public.device_command_state (device_id, request_id)
    where request_id is not null and state in ('delivered', 'accepted');

alter table public.device_command_state enable row level security;

revoke all on table public.device_command_state from public, anon, authenticated;
grant select, insert, update, delete on table public.device_command_state to service_role;

create function public.device_command_receipt(
    p_cmd_id bigint,
    p_phase smallint,
    p_result smallint,
    p_receipt smallint,
    p_error smallint
)
returns jsonb
language sql
immutable
strict
security invoker
set search_path = ''
as $function$
    select jsonb_build_array(
        p_cmd_id,
        p_phase,
        p_result,
        p_receipt,
        p_error
    );
$function$;

revoke all on function public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)
    from public, anon, authenticated;
grant execute on function public.device_command_receipt(bigint,smallint,smallint,smallint,smallint)
    to service_role;

insert into public.device_command_state (
    cmd_id,
    device_id,
    opcode,
    job_id,
    state,
    request_id,
    ttl_seconds,
    delivery_attempts,
    lease_expires_at,
    created_at,
    expires_at,
    last_delivered_at,
    final_at,
    final_result,
    final_error,
    legacy_status,
    updated_at
)
select
    legacy."cmdId",
    legacy."deviceId",
    legacy.cmd,
    legacy."cmdId"::bigint,
    case
        when normalized.expires_at <= statement_timestamp() then 'expired'
        when legacy.status = 0 then 'queued'
        else 'delivered'
    end,
    null,
    86400,
    case when legacy.status = 1 then 1 else 0 end,
    case
        when legacy.status = 1
            then normalized.delivered_at + interval '60 seconds'
        else null
    end,
    normalized.created_at,
    normalized.expires_at,
    case when legacy.status = 1 then normalized.delivered_at else null end,
    case
        when normalized.expires_at <= statement_timestamp()
            then normalized.expires_at
        else null
    end,
    case
        when normalized.expires_at <= statement_timestamp() then 4
        else null
    end,
    case
        when normalized.expires_at <= statement_timestamp() then 5
        else null
    end,
    legacy.status,
    statement_timestamp()
from public."deviceCmds" as legacy
cross join lateral (
    select
        coalesce(
            legacy.created_at,
            statement_timestamp() at time zone 'Asia/Seoul'
        ) at time zone 'Asia/Seoul' as created_at,
        coalesce(
            legacy.sent_at,
            legacy.created_at,
            statement_timestamp() at time zone 'Asia/Seoul'
        ) at time zone 'Asia/Seoul' as delivered_at,
        (
            coalesce(
                legacy.created_at,
                statement_timestamp() at time zone 'Asia/Seoul'
            ) at time zone 'Asia/Seoul'
        ) + interval '1 day' as expires_at
) as normalized;

create function public.sync_device_command_state()
returns trigger
language plpgsql
security definer
set search_path = ''
as $function$
declare
    v_created_at timestamp with time zone;
    v_delivered_at timestamp with time zone;
    v_expires_at timestamp with time zone;
    v_now timestamp with time zone;
begin
    v_now := clock_timestamp();
    v_created_at := coalesce(
        new.created_at,
        v_now at time zone 'Asia/Seoul'
    ) at time zone 'Asia/Seoul';
    v_delivered_at := coalesce(
        new.sent_at,
        new.created_at,
        v_now at time zone 'Asia/Seoul'
    ) at time zone 'Asia/Seoul';
    v_expires_at := v_created_at + interval '1 day';

    insert into public.device_command_state (
        cmd_id,
        device_id,
        opcode,
        job_id,
        state,
        request_id,
        ttl_seconds,
        delivery_attempts,
        lease_expires_at,
        created_at,
        expires_at,
        last_delivered_at,
        final_at,
        final_result,
        final_error,
        legacy_status,
        updated_at
    )
    values (
        new."cmdId",
        new."deviceId",
        new.cmd,
        new."cmdId"::bigint,
        case
            when v_expires_at <= v_now then 'expired'
            when new.status = 0 then 'queued'
            else 'delivered'
        end,
        null,
        86400,
        case when new.status = 1 then 1 else 0 end,
        case
            when new.status = 1 then v_delivered_at + interval '60 seconds'
            else null
        end,
        v_created_at,
        v_expires_at,
        case when new.status = 1 then v_delivered_at else null end,
        case when v_expires_at <= v_now then v_expires_at else null end,
        case when v_expires_at <= v_now then 4 else null end,
        case when v_expires_at <= v_now then 5 else null end,
        new.status,
        v_now
    )
    on conflict (cmd_id) do update
    set
        device_id = excluded.device_id,
        opcode = excluded.opcode,
        legacy_status = excluded.legacy_status,
        state = case
            when public.device_command_state.state = 'queued'
                 and excluded.state = 'delivered'
                then 'delivered'
            else public.device_command_state.state
        end,
        delivery_attempts = case
            when public.device_command_state.state = 'queued'
                 and excluded.state = 'delivered'
                then greatest(public.device_command_state.delivery_attempts, 1)
            else public.device_command_state.delivery_attempts
        end,
        last_delivered_at = case
            when public.device_command_state.state = 'queued'
                 and excluded.state = 'delivered'
                then excluded.last_delivered_at
            else public.device_command_state.last_delivered_at
        end,
        lease_expires_at = case
            when public.device_command_state.state = 'queued'
                 and excluded.state = 'delivered'
                then excluded.lease_expires_at
            else public.device_command_state.lease_expires_at
        end,
        updated_at = v_now;

    return new;
end;
$function$;

revoke all on function public.sync_device_command_state()
    from public, anon, authenticated;

create trigger trg_sync_device_command_state
after insert or update of status, sent_at, cmd, "deviceId"
on public."deviceCmds"
for each row
execute function public.sync_device_command_state();

create function public.claim_device_command(
    p_imei text,
    p_request_id bigint,
    p_last_cmd_id bigint,
    p_lease_seconds integer default 60
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $function$
declare
    v_device_id integer;
    v_state public.device_command_state%rowtype;
    v_ttl_seconds integer;
begin
    if p_imei is null
       or length(btrim(p_imei)) = 0
       or length(p_imei) > 32
       or p_request_id is null
       or p_request_id not between 1 and 4294967295
       or p_last_cmd_id is null
       or p_last_cmd_id not between 0 and 4294967295
       or p_lease_seconds is null
       or p_lease_seconds not between 5 and 300 then
        raise exception using
            errcode = '22023',
            message = 'invalid command claim input';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_trigger as t
        where t.tgrelid = 'public.sensorvalue'::regclass
          and t.tgname = 'trg_assign_device_command'
          and not t.tgisinternal
    ) then
        raise exception using
            errcode = '55000',
            message = 'legacy command path remains active';
    end if;

    begin
        select d."deviceId"
        into strict v_device_id
        from public.device as d
        where d."deviceIMEI" = p_imei
        for update;
    exception
        when no_data_found then
            return jsonb_build_array(p_request_id, 0, 0, 0, 0);
    end;

    update public.device_command_state as s
    set
        state = 'executed',
        final_at = coalesce(s.final_at, clock_timestamp()),
        final_result = 2,
        final_error = 0,
        lease_expires_at = null,
        updated_at = clock_timestamp()
    where s.device_id = v_device_id
      and s.cmd_id <= p_last_cmd_id
      and s.state in ('queued', 'delivered', 'accepted');

    update public.device_command_state as s
    set
        state = 'expired',
        final_at = s.expires_at,
        final_result = 4,
        final_error = 5,
        lease_expires_at = null,
        updated_at = clock_timestamp()
    where s.device_id = v_device_id
      and s.state in ('queued', 'delivered')
      and s.expires_at <= clock_timestamp();

    update public.device_command_state as s
    set
        state = 'delivered',
        delivery_attempts = greatest(s.delivery_attempts, 1),
        last_delivered_at = coalesce(
            s.last_delivered_at,
            c.sent_at at time zone 'Asia/Seoul',
            clock_timestamp()
        ),
        lease_expires_at = coalesce(
            s.lease_expires_at,
            (c.sent_at at time zone 'Asia/Seoul') + make_interval(secs => p_lease_seconds),
            clock_timestamp()
        ),
        legacy_status = c.status,
        updated_at = clock_timestamp()
    from public."deviceCmds" as c
    where c."cmdId" = s.cmd_id
      and s.device_id = v_device_id
      and s.state = 'queued'
      and c.status = 1;

    select s.*
    into v_state
    from public.device_command_state as s
    where s.device_id = v_device_id
      and s.request_id = p_request_id
      and s.state in ('delivered', 'accepted')
    order by s.updated_at desc
    limit 1
    for update;

    if found then
        v_ttl_seconds := greatest(
            1,
            least(
                86400,
                ceil(extract(epoch from (v_state.expires_at - clock_timestamp())))::integer
            )
        );
        return jsonb_build_array(
            p_request_id,
            v_state.cmd_id,
            v_state.opcode,
            v_state.job_id,
            v_ttl_seconds
        );
    end if;

    select s.*
    into v_state
    from public.device_command_state as s
    join public."deviceCmds" as c
      on c."cmdId" = s.cmd_id
    where s.device_id = v_device_id
      and s.cmd_id > p_last_cmd_id
      and s.expires_at > clock_timestamp()
      and (
          (s.state = 'queued' and c.status = 0)
          or
          (
              s.state = 'delivered'
              and s.lease_expires_at <= clock_timestamp()
              and s.delivery_attempts < 5
          )
      )
    order by s.cmd_id asc
    limit 1
    for update of s, c skip locked;

    if not found then
        return jsonb_build_array(p_request_id, 0, 0, 0, 0);
    end if;

    update public.device_command_state
    set
        state = 'delivered',
        request_id = p_request_id,
        delivery_attempts = delivery_attempts + 1,
        last_delivered_at = clock_timestamp(),
        lease_expires_at = clock_timestamp() + make_interval(secs => p_lease_seconds),
        legacy_status = 1,
        updated_at = clock_timestamp()
    where cmd_id = v_state.cmd_id
    returning * into v_state;

    update public."deviceCmds"
    set
        status = 1,
        sent_at = coalesce(sent_at, clock_timestamp() at time zone 'Asia/Seoul')
    where "cmdId" = v_state.cmd_id
      and status = 0;

    v_ttl_seconds := greatest(
        1,
        least(
            86400,
            ceil(extract(epoch from (v_state.expires_at - clock_timestamp())))::integer
        )
    );

    return jsonb_build_array(
        p_request_id,
        v_state.cmd_id,
        v_state.opcode,
        v_state.job_id,
        v_ttl_seconds
    );
end;
$function$;

revoke all on function public.claim_device_command(text,bigint,bigint,integer)
    from public, anon, authenticated;
grant execute on function public.claim_device_command(text,bigint,bigint,integer)
    to service_role;

create function public.ack_device_command(
    p_imei text,
    p_cmd_id bigint,
    p_phase smallint,
    p_result smallint,
    p_error smallint,
    p_unix_seconds bigint,
    p_clock_valid boolean
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $function$
declare
    v_state public.device_command_state%rowtype;
    receipt_ingested constant smallint := 1;
    receipt_rejected constant smallint := 2;
    receipt_mismatch constant smallint := 3;
    receipt_error_none constant smallint := 0;
    receipt_error_invalid constant smallint := 1;
    receipt_error_unknown_command constant smallint := 2;
    receipt_error_state constant smallint := 3;
    receipt_error_mismatch constant smallint := 4;
begin
    if p_imei is null
       or length(btrim(p_imei)) = 0
       or length(p_imei) > 32
       or p_cmd_id is null
       or p_cmd_id <= 0
       or p_cmd_id > 4294967295
       or p_phase is null
       or p_phase not between 1 and 2
       or p_result is null
       or p_result not between 1 and 4
       or p_error is null
       or p_error not between 0 and 5
       or p_unix_seconds is null
       or p_unix_seconds not between 0 and 4294967295
       or p_clock_valid is null then
        return public.device_command_receipt(
            greatest(coalesce(p_cmd_id, 0), 0),
            greatest(coalesce(p_phase, 0), 0)::smallint,
            greatest(coalesce(p_result, 0), 0)::smallint,
            receipt_rejected,
            receipt_error_invalid
        );
    end if;

    if (
        p_phase = 1
        and not (p_result = 1 and p_error = 0)
    ) or (
        p_phase = 2
        and not (
            (p_result = 2 and p_error = 0)
            or (p_result = 3 and p_error between 1 and 4)
            or (p_result = 4 and p_error = 5)
        )
    ) then
        return public.device_command_receipt(
            p_cmd_id,
            p_phase,
            p_result,
            receipt_rejected,
            receipt_error_invalid
        );
    end if;

    select s.*
    into v_state
    from public.device_command_state as s
    join public.device as d
      on d."deviceId" = s.device_id
    where s.cmd_id = p_cmd_id
      and d."deviceIMEI" = p_imei
    for update of s;

    if not found then
        return public.device_command_receipt(
            p_cmd_id,
            p_phase,
            p_result,
            receipt_rejected,
            receipt_error_unknown_command
        );
    end if;

    if p_phase = 1 then
        if v_state.accepted_result is not null then
            if v_state.accepted_result = p_result
               and v_state.accepted_error = p_error then
                return public.device_command_receipt(
                    p_cmd_id,
                    p_phase,
                    p_result,
                    receipt_ingested,
                    receipt_error_none
                );
            end if;
            return public.device_command_receipt(
                p_cmd_id,
                p_phase,
                p_result,
                receipt_mismatch,
                receipt_error_mismatch
            );
        end if;

        if v_state.state <> 'delivered' then
            return public.device_command_receipt(
                p_cmd_id,
                p_phase,
                p_result,
                receipt_mismatch,
                receipt_error_state
            );
        end if;

        update public.device_command_state
        set
            state = 'accepted',
            accepted_at = clock_timestamp(),
            accepted_result = p_result,
            accepted_error = p_error,
            accepted_unix_seconds = p_unix_seconds,
            accepted_clock_valid = p_clock_valid,
            lease_expires_at = null,
            updated_at = clock_timestamp()
        where cmd_id = p_cmd_id;

        return public.device_command_receipt(
            p_cmd_id,
            p_phase,
            p_result,
            receipt_ingested,
            receipt_error_none
        );
    end if;

    if v_state.final_result is not null then
        if v_state.final_result = p_result
           and v_state.final_error = p_error then
            return public.device_command_receipt(
                p_cmd_id,
                p_phase,
                p_result,
                receipt_ingested,
                receipt_error_none
            );
        end if;
        return public.device_command_receipt(
            p_cmd_id,
            p_phase,
            p_result,
            receipt_mismatch,
            receipt_error_mismatch
        );
    end if;

    if v_state.state <> 'accepted'
       or v_state.accepted_result <> 1
       or v_state.accepted_error <> 0 then
        return public.device_command_receipt(
            p_cmd_id,
            p_phase,
            p_result,
            receipt_mismatch,
            receipt_error_state
        );
    end if;

    update public.device_command_state
    set
        state = case p_result
            when 2 then 'executed'
            when 3 then 'failed'
            else 'expired'
        end,
        final_at = clock_timestamp(),
        final_result = p_result,
        final_error = p_error,
        final_unix_seconds = p_unix_seconds,
        final_clock_valid = p_clock_valid,
        updated_at = clock_timestamp()
    where cmd_id = p_cmd_id;

    return public.device_command_receipt(
        p_cmd_id,
        p_phase,
        p_result,
        receipt_ingested,
        receipt_error_none
    );
end;
$function$;

revoke all on function public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)
    from public, anon, authenticated;
grant execute on function public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)
    to service_role;

comment on table public.device_command_state is
    'Additive command delivery/ACK companion for legacy public."deviceCmds".';
comment on function public.claim_device_command(text,bigint,bigint,integer) is
    'Claims at most one command and returns [request_id,cmd_id,opcode,job_id,ttl_seconds].';
comment on function public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean) is
    'Idempotently stores accepted/final ACK and returns [cmd_id,phase,result,receipt,error].';

commit;
