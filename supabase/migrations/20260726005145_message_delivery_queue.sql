begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

create table public.alert_contact (
    alert_contact_id bigint generated always as identity primary key,
    user_id integer not null,
    destination_e164 text not null,
    consent_status text not null,
    consent_scope text not null,
    consent_source text not null,
    consent_recorded_at timestamptz not null,
    consent_revoked_at timestamptz,
    provenance_source text not null,
    is_active boolean not null default true,
    created_at timestamptz not null default statement_timestamp(),
    updated_at timestamptz not null default statement_timestamp(),
    constraint alert_contact_destination_check
        check (destination_e164 ~ '^\+[1-9][0-9]{7,14}$'),
    constraint alert_contact_consent_status_check
        check (consent_status in ('granted', 'revoked')),
    constraint alert_contact_consent_state_check
        check (
            (
                consent_status = 'granted'
                and consent_revoked_at is null
            )
            or
            (
                consent_status = 'revoked'
                and consent_revoked_at is not null
                and consent_revoked_at >= consent_recorded_at
            )
        ),
    constraint alert_contact_active_consent_check
        check (not is_active or consent_status = 'granted'),
    constraint alert_contact_text_check
        check (
            pg_catalog.length(consent_scope) between 1 and 64
            and pg_catalog.length(consent_source) between 1 and 128
            and pg_catalog.length(provenance_source) between 1 and 128
        ),
    constraint alert_contact_time_check
        check (updated_at >= created_at),
    constraint alert_contact_identity_unique
        unique (user_id, destination_e164, consent_scope)
);

create table public.message_policy (
    message_policy_id bigint generated always as identity primary key,
    policy_key text not null,
    primary_channel text not null default 'alimtalk',
    allow_sms_fallback boolean not null default false,
    max_attempts integer not null default 5,
    lease_seconds integer not null default 60,
    retry_base_seconds integer not null default 30,
    retry_max_seconds integer not null default 3600,
    monthly_cost_cap numeric(14, 2) not null default 0,
    cost_warning_ratio numeric(5, 4) not null default 0.8000,
    primary_cost_class text not null,
    fallback_cost_class text,
    provenance_source text not null,
    is_active boolean not null default true,
    effective_from timestamptz not null default statement_timestamp(),
    effective_until timestamptz,
    created_at timestamptz not null default statement_timestamp(),
    updated_at timestamptz not null default statement_timestamp(),
    constraint message_policy_key_unique unique (policy_key),
    constraint message_policy_channel_check
        check (primary_channel = 'alimtalk'),
    constraint message_policy_retry_bounds_check
        check (
            max_attempts between 1 and 10
            and lease_seconds between 15 and 300
            and retry_base_seconds between 1 and 3600
            and retry_max_seconds between retry_base_seconds and 86400
        ),
    constraint message_policy_fallback_check
        check (
            (allow_sms_fallback and fallback_cost_class is not null)
            or
            (not allow_sms_fallback and fallback_cost_class is null)
        ),
    constraint message_policy_cost_check
        check (
            monthly_cost_cap >= 0
            and cost_warning_ratio > 0
            and cost_warning_ratio <= 1
            and pg_catalog.length(primary_cost_class) between 1 and 64
            and (
                fallback_cost_class is null
                or pg_catalog.length(fallback_cost_class) between 1 and 64
            )
        ),
    constraint message_policy_text_check
        check (
            pg_catalog.length(policy_key) between 1 and 128
            and pg_catalog.length(provenance_source) between 1 and 128
        ),
    constraint message_policy_time_check
        check (
            updated_at >= created_at
            and (
                effective_until is null
                or effective_until > effective_from
            )
        )
);

create table public.msg_send (
    msg_send_id bigint generated always as identity primary key,
    alert_contact_id bigint not null
        references public.alert_contact (alert_contact_id) on delete restrict,
    message_policy_id bigint not null
        references public.message_policy (message_policy_id) on delete restrict,
    source_user_id integer not null,
    source_device_id integer not null,
    source_event_origin text not null,
    source_event_type text not null,
    source_event_sequence bigint not null,
    incident_id bigint not null,
    incident_kind text not null,
    template_code text not null,
    template_stage text not null,
    template_params jsonb not null default '{}'::jsonb,
    dedupe_key text not null,
    status text not null default 'pending',
    active_channel text not null default 'alimtalk',
    priority smallint not null default 0,
    available_at timestamptz not null default statement_timestamp(),
    expires_at timestamptz not null,
    lock_owner text,
    lease_token uuid,
    lease_generation bigint not null default 0,
    lease_expires_at timestamptz,
    attempt_count integer not null default 0,
    max_attempts integer not null,
    last_attempt_at timestamptz,
    provider text not null default 'bizppurio',
    provider_submission_started_at timestamptz,
    provider_submission_lease_token uuid,
    provider_request_id text,
    provider_accepted_at timestamptz,
    provider_result_id text,
    provider_result_status text,
    provider_result_retryable boolean,
    provider_result_code text,
    provider_result_at timestamptz,
    provider_cost numeric(14, 4),
    fallback_provider text,
    fallback_submission_started_at timestamptz,
    fallback_submission_lease_token uuid,
    fallback_provider_request_id text,
    fallback_provider_accepted_at timestamptz,
    fallback_provider_result_id text,
    fallback_provider_result_status text,
    fallback_provider_result_retryable boolean,
    fallback_provider_result_code text,
    fallback_provider_result_at timestamptz,
    fallback_provider_cost numeric(14, 4),
    delivered_channel text,
    fallback_reason text,
    cost_class text,
    estimated_cost numeric(14, 4),
    final_cost numeric(14, 4),
    cost_currency text not null default 'KRW',
    last_error_code text,
    sent_at timestamptz,
    terminal_at timestamptz,
    created_at timestamptz not null default statement_timestamp(),
    updated_at timestamptz not null default statement_timestamp(),
    constraint msg_send_state_check
        check (
            status in (
                'pending',
                'processing',
                'accepted',
                'waiting_result',
                'retry_wait',
                'sent',
                'failed',
                'suppressed',
                'cancelled'
            )
        ),
    constraint msg_send_active_channel_check
        check (active_channel in ('alimtalk', 'sms')),
    constraint msg_send_delivered_channel_check
        check (
            delivered_channel is null
            or delivered_channel in ('alimtalk', 'sms')
        ),
    constraint msg_send_provenance_check
        check (
            source_event_sequence >= 0
            and incident_id > 0
            and pg_catalog.length(source_event_origin) between 1 and 64
            and pg_catalog.length(source_event_type) between 1 and 64
            and pg_catalog.length(incident_kind) between 1 and 64
        ),
    constraint msg_send_template_check
        check (
            pg_catalog.length(template_code) between 1 and 128
            and pg_catalog.length(template_stage) between 1 and 64
            and pg_catalog.jsonb_typeof(template_params) = 'object'
        ),
    constraint msg_send_attempt_check
        check (
            max_attempts between 1 and 10
            and attempt_count between 0 and max_attempts
                + case when active_channel = 'sms' then 1 else 0 end
        ),
    constraint msg_send_priority_check
        check (priority between 0 and 100),
    constraint msg_send_lease_state_check
        check (
            (
                status in ('processing', 'accepted')
                and lock_owner is not null
                and lease_token is not null
                and lease_generation > 0
                and lease_expires_at is not null
                and last_attempt_at is not null
            )
            or
            (
                status not in ('processing', 'accepted')
                and lock_owner is null
                and lease_token is null
                and lease_expires_at is null
            )
        ),
    constraint msg_send_provider_result_check
        check (
            (
                provider_submission_started_at is null
                and provider_submission_lease_token is null
                and provider_request_id is null
                and provider_accepted_at is null
                and provider_result_id is null
                and provider_result_status is null
                and provider_result_retryable is null
                and provider_result_code is null
                and provider_result_at is null
                and provider_cost is null
            )
            or
            (
                provider_submission_started_at is not null
                and provider_submission_lease_token is not null
                and (
                    (
                        provider_request_id is null
                        and provider_accepted_at is null
                        and provider_result_id is null
                        and provider_result_status is null
                        and provider_result_retryable is null
                        and provider_result_code is null
                        and provider_result_at is null
                        and provider_cost is null
                    )
                    or
                    (
                        provider_request_id is not null
                        and provider_accepted_at is not null
                        and (
                            (
                                provider_result_id is null
                                and provider_result_status is null
                                and provider_result_retryable is null
                                and provider_result_code is null
                                and provider_result_at is null
                                and provider_cost is null
                            )
                            or
                            (
                                provider_result_id is not null
                                and provider_result_status in (
                                    'success',
                                    'failure'
                                )
                                and provider_result_retryable is not null
                                and provider_result_code is not null
                                and provider_result_at is not null
                                and provider_cost is not null
                                and provider_cost >= 0
                            )
                        )
                    )
                )
            )
        ),
    constraint msg_send_fallback_result_check
        check (
            (
                fallback_submission_started_at is null
                and fallback_submission_lease_token is null
                and fallback_provider_request_id is null
                and fallback_provider_accepted_at is null
                and fallback_provider_result_id is null
                and fallback_provider_result_status is null
                and fallback_provider_result_retryable is null
                and fallback_provider_result_code is null
                and fallback_provider_result_at is null
                and fallback_provider_cost is null
            )
            or
            (
                fallback_provider = 'bizppurio'
                and fallback_reason is not null
                and fallback_submission_started_at is not null
                and fallback_submission_lease_token is not null
                and (
                    (
                        fallback_provider_request_id is null
                        and fallback_provider_accepted_at is null
                        and fallback_provider_result_id is null
                        and fallback_provider_result_status is null
                        and fallback_provider_result_retryable is null
                        and fallback_provider_result_code is null
                        and fallback_provider_result_at is null
                        and fallback_provider_cost is null
                    )
                    or
                    (
                        fallback_provider_request_id is not null
                        and fallback_provider_accepted_at is not null
                        and (
                            (
                                fallback_provider_result_id is null
                                and fallback_provider_result_status is null
                                and fallback_provider_result_retryable is null
                                and fallback_provider_result_code is null
                                and fallback_provider_result_at is null
                                and fallback_provider_cost is null
                            )
                            or
                            (
                                fallback_provider_result_id is not null
                                and fallback_provider_result_status in (
                                    'success',
                                    'failure'
                                )
                                and fallback_provider_result_retryable
                                    is not null
                                and fallback_provider_result_code is not null
                                and fallback_provider_result_at is not null
                                and fallback_provider_cost is not null
                                and fallback_provider_cost >= 0
                            )
                        )
                    )
                )
            )
        ),
    constraint msg_send_submission_state_check
        check (
            (
                status = 'accepted'
                and (
                    (
                        active_channel = 'alimtalk'
                        and provider_submission_started_at is not null
                        and provider_submission_lease_token is not null
                        and provider_request_id is null
                        and provider_accepted_at is null
                        and provider_result_id is null
                        and provider_result_status is null
                        and provider_result_retryable is null
                        and provider_result_code is null
                        and provider_result_at is null
                        and provider_cost is null
                    )
                    or
                    (
                        active_channel = 'sms'
                        and fallback_submission_started_at is not null
                        and fallback_submission_lease_token is not null
                        and fallback_provider_request_id is null
                        and fallback_provider_accepted_at is null
                        and fallback_provider_result_id is null
                    )
                )
            )
            or
            (
                status = 'waiting_result'
                and (
                    (
                        active_channel = 'alimtalk'
                        and provider_submission_started_at is not null
                        and provider_request_id is not null
                        and provider_accepted_at is not null
                        and provider_submission_lease_token is not null
                        and provider_result_id is null
                    )
                    or
                    (
                        active_channel = 'sms'
                        and fallback_submission_started_at is not null
                        and fallback_provider_request_id is not null
                        and fallback_provider_accepted_at is not null
                        and fallback_submission_lease_token is not null
                        and fallback_provider_result_id is null
                    )
                )
            )
            or status not in ('accepted', 'waiting_result')
        ),
    constraint msg_send_terminal_state_check
        check (
            (
                status = 'sent'
                and sent_at is not null
                and terminal_at is not null
                and delivered_channel is not null
                and final_cost is not null
                and final_cost >= 0
            )
            or
            (
                status in ('failed', 'suppressed', 'cancelled')
                and sent_at is null
                and terminal_at is not null
                and delivered_channel is null
                and (final_cost is null or final_cost >= 0)
            )
            or
            (
                status not in ('sent', 'failed', 'suppressed', 'cancelled')
                and sent_at is null
                and terminal_at is null
                and delivered_channel is null
            )
        ),
    constraint msg_send_channel_state_check
        check (
            (
                active_channel = 'alimtalk'
                and fallback_reason is null
            )
            or
            (
                active_channel = 'sms'
                and fallback_reason is not null
                and pg_catalog.length(fallback_reason) between 1 and 128
                and provider_result_status is not distinct from 'failure'
            )
        ),
    constraint msg_send_delivery_state_check
        check (
            delivered_channel is null
            or
            (
                delivered_channel = 'alimtalk'
                and active_channel = 'alimtalk'
                and provider_result_status is not distinct from 'success'
            )
            or
            (
                delivered_channel = 'sms'
                and active_channel = 'sms'
                and fallback_provider_result_status
                    is not distinct from 'success'
            )
        ),
    constraint msg_send_time_check
        check (
            expires_at > created_at
            and available_at < expires_at
            and updated_at >= created_at
            and (
                sent_at is null
                or sent_at >= created_at
            )
            and (
                terminal_at is null
                or terminal_at >= created_at
            )
        ),
    constraint msg_send_cost_check
        check (
            estimated_cost is null or estimated_cost >= 0
        ),
    constraint msg_send_currency_check
        check (cost_currency ~ '^[A-Z]{3}$'),
    constraint msg_send_dedupe_key_unique unique (dedupe_key),
    constraint msg_send_incident_template_stage_unique
        unique (
            source_device_id,
            incident_id,
            template_code,
            template_stage,
            alert_contact_id
        )
);

create table public.message_link_token (
    message_link_token_id bigint generated always as identity primary key,
    msg_send_id bigint not null
        references public.msg_send (msg_send_id) on delete restrict,
    token_hash bytea not null,
    token_hash_algorithm text not null default 'sha256',
    purpose text not null,
    target_path text not null,
    expires_at timestamptz not null,
    consumed_at timestamptz,
    revoked_at timestamptz,
    created_at timestamptz not null default statement_timestamp(),
    constraint message_link_token_token_hash_unique unique (token_hash),
    constraint message_link_token_hash_algorithm_check
        check (token_hash_algorithm = 'sha256'),
    constraint message_link_token_hash_length_check
        check (pg_catalog.octet_length(token_hash) = 32),
    constraint message_link_token_target_check
        check (
            pg_catalog.left(target_path, 1) = '/'
            and pg_catalog.left(target_path, 2) <> '//'
        ),
    constraint message_link_token_time_check
        check (
            expires_at > created_at
            and (
                consumed_at is null
                or consumed_at >= created_at
            )
            and (
                revoked_at is null
                or revoked_at >= created_at
            )
        )
);

create index idx_msg_send_claim
    on public.msg_send (
        status,
        available_at,
        priority desc,
        msg_send_id
    )
    where status in ('pending', 'retry_wait');

create index idx_msg_send_lease
    on public.msg_send (lease_expires_at, msg_send_id)
    where status in ('processing', 'accepted');

create unique index uq_msg_send_provider_request
    on public.msg_send (provider, provider_request_id)
    where provider_request_id is not null;

create unique index uq_msg_send_fallback_provider_request
    on public.msg_send (fallback_provider, fallback_provider_request_id)
    where fallback_provider_request_id is not null;

create unique index uq_msg_send_provider_result
    on public.msg_send (provider, provider_result_id)
    where provider_result_id is not null;

create unique index uq_msg_send_fallback_provider_result
    on public.msg_send (fallback_provider, fallback_provider_result_id)
    where fallback_provider_result_id is not null;

create index idx_message_link_token_expiry
    on public.message_link_token (expires_at)
    where consumed_at is null and revoked_at is null;

alter table public.alert_contact enable row level security;
alter table public.message_policy enable row level security;
alter table public.msg_send enable row level security;
alter table public.message_link_token enable row level security;

revoke all on table public.alert_contact
    from public, anon, authenticated, service_role;
revoke all on table public.message_policy
    from public, anon, authenticated, service_role;
revoke all on table public.msg_send
    from public, anon, authenticated, service_role;
revoke all on table public.message_link_token
    from public, anon, authenticated, service_role;

grant select, insert, update on table public.alert_contact
    to service_role;
grant select, insert, update on table public.message_policy
    to service_role;
grant select, insert on table public.msg_send
    to service_role;
grant select, insert, update on table public.message_link_token
    to service_role;

revoke all on sequence public.alert_contact_alert_contact_id_seq
    from public, anon, authenticated, service_role;
revoke all on sequence public.message_policy_message_policy_id_seq
    from public, anon, authenticated, service_role;
revoke all on sequence public.msg_send_msg_send_id_seq
    from public, anon, authenticated, service_role;
revoke all on sequence public.message_link_token_message_link_token_id_seq
    from public, anon, authenticated, service_role;

grant usage, select on sequence public.alert_contact_alert_contact_id_seq
    to service_role;
grant usage, select on sequence public.message_policy_message_policy_id_seq
    to service_role;
grant usage, select on sequence public.msg_send_msg_send_id_seq
    to service_role;
grant usage, select on sequence public.message_link_token_message_link_token_id_seq
    to service_role;

-- SECURITY DEFINER is required to keep claim-and-lease atomic behind a
-- service_role-only RPC while allowing a future worker role without direct
-- queue UPDATE privilege. All relations are schema-qualified.
create function public.claim_msg_send(
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
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner)) not between 1 and 128 then
        raise exception 'invalid lock owner'
            using errcode = '22023';
    end if;
    if p_batch_size is null or p_batch_size not between 1 and 50 then
        raise exception 'batch size must be between 1 and 50'
            using errcode = '22023';
    end if;
    if p_lease_seconds is null or p_lease_seconds not between 15 and 300 then
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
                v_now + pg_catalog.make_interval(secs => p_lease_seconds),
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

-- The first production send uses a stronger, exact claim contract. A table
-- writer lock closes the queue-cardinality phantom window while this function
-- verifies the sole active row, its immutable one-attempt Alimtalk policy, and
-- the expected identity before creating the claim lease.
create function public.claim_exact_one_shot_msg_send(
    p_expected_msg_send_id bigint,
    p_lock_owner text,
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
    v_active_count bigint;
    v_active_msg_send_id bigint;
    v_message public.msg_send%rowtype;
begin
    if p_expected_msg_send_id is null
       or p_expected_msg_send_id <= 0 then
        raise exception 'invalid expected message id'
            using errcode = '22023';
    end if;
    if p_lock_owner is null
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner))
          not between 1 and 128 then
        raise exception 'invalid lock owner'
            using errcode = '22023';
    end if;
    if p_lease_seconds is null or p_lease_seconds not between 15 and 300 then
        raise exception 'lease seconds must be between 15 and 300'
            using errcode = '22023';
    end if;

    lock table public.msg_send in share row exclusive mode;

    v_now := pg_catalog.clock_timestamp();

    select
        pg_catalog.count(*),
        pg_catalog.min(m.msg_send_id)
    into
        v_active_count,
        v_active_msg_send_id
    from public.msg_send as m
    where m.status in (
        'pending',
        'retry_wait',
        'processing',
        'accepted',
        'waiting_result'
    );

    if v_active_count <> 1 then
        raise exception 'one-shot active queue cardinality must equal one'
            using errcode = '55000';
    end if;
    if v_active_msg_send_id is distinct from p_expected_msg_send_id then
        raise exception 'one-shot active queue identity mismatch'
            using errcode = '55000';
    end if;

    select m.*
    into v_message
    from public.msg_send as m
    join public.message_policy as p
      on p.message_policy_id = m.message_policy_id
    where m.msg_send_id = p_expected_msg_send_id
      and m.status in ('pending', 'retry_wait')
      and m.available_at <= v_now
      and m.expires_at > v_now
      and m.attempt_count = 0
      and m.max_attempts = 1
      and m.active_channel = 'alimtalk'
      and m.fallback_reason is null
      and p.is_active
      and not p.allow_sms_fallback
      and p.fallback_cost_class is null
    for update of m, p;

    if not found then
        raise exception 'one-shot message safety contract mismatch'
            using errcode = '55000';
    end if;

    update public.msg_send as m
    set
        status = 'processing',
        lock_owner = p_lock_owner,
        lease_token = pg_catalog.gen_random_uuid(),
        lease_generation = m.lease_generation + 1,
        lease_expires_at =
            v_now + pg_catalog.make_interval(secs => p_lease_seconds),
        attempt_count = m.attempt_count + 1,
        last_attempt_at = v_now,
        updated_at = v_now
    where m.msg_send_id = p_expected_msg_send_id
    returning m.* into strict v_message;

    return next v_message;
    return;
end;
$function$;

revoke all on function public.claim_exact_one_shot_msg_send(
    bigint,text,integer
) from public, anon, authenticated;
grant execute on function public.claim_exact_one_shot_msg_send(
    bigint,text,integer
) to service_role;

-- This transition is committed before the provider call. The active channel,
-- opaque attempt token, database start time, and claim fence become durable
-- together, so an accepted-but-unregistered provider outcome is never blindly
-- resent.
create function public.mark_msg_send_submission_started(
    p_msg_send_id bigint,
    p_lock_owner text,
    p_lease_token uuid,
    p_channel text
)
returns jsonb
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_message public.msg_send%rowtype;
    v_now timestamptz;
begin
    if p_msg_send_id is null or p_msg_send_id <= 0 then
        raise exception 'invalid message id'
            using errcode = '22023';
    end if;
    if p_lock_owner is null
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner))
          not between 1 and 128
       or p_lease_token is null then
        raise exception 'invalid claim fence'
            using errcode = '22023';
    end if;
    if p_channel is null or p_channel not in ('alimtalk', 'sms') then
        raise exception 'invalid submission channel'
            using errcode = '22023';
    end if;
    v_now := pg_catalog.clock_timestamp();

    select m.*
    into strict v_message
    from public.msg_send as m
    where m.msg_send_id = p_msg_send_id
    for update;

    if v_message.status is distinct from 'processing'
       or v_message.lock_owner is distinct from p_lock_owner
       or v_message.lease_token is distinct from p_lease_token
       or v_message.lease_expires_at is null
       or v_message.lease_expires_at <= v_now then
        raise exception 'stale or expired claim fence'
            using errcode = '55000';
    end if;
    if v_message.active_channel is distinct from p_channel then
        raise exception 'submission channel mismatch'
            using errcode = '22000';
    end if;

    if p_channel = 'alimtalk' then
        update public.msg_send as m
        set
            status = 'accepted',
            provider_submission_started_at = v_now,
            provider_submission_lease_token = p_lease_token,
            provider_request_id = null,
            provider_accepted_at = null,
            provider_result_id = null,
            provider_result_status = null,
            provider_result_retryable = null,
            provider_result_code = null,
            provider_result_at = null,
            provider_cost = null,
            updated_at = v_now
        where m.msg_send_id = p_msg_send_id
        returning m.* into strict v_message;
    else
        update public.msg_send as m
        set
            status = 'accepted',
            fallback_provider = 'bizppurio',
            fallback_submission_started_at = v_now,
            fallback_submission_lease_token = p_lease_token,
            fallback_provider_request_id = null,
            fallback_provider_accepted_at = null,
            fallback_provider_result_id = null,
            fallback_provider_result_status = null,
            fallback_provider_result_retryable = null,
            fallback_provider_result_code = null,
            fallback_provider_result_at = null,
            fallback_provider_cost = null,
            updated_at = v_now
        where m.msg_send_id = p_msg_send_id
        returning m.* into strict v_message;
    end if;

    return pg_catalog.to_jsonb(v_message);
end;
$function$;

revoke all on function public.mark_msg_send_submission_started(
    bigint,text,uuid,text
) from public, anon, authenticated;
grant execute on function public.mark_msg_send_submission_started(
    bigint,text,uuid,text
) to service_role;

-- SECURITY DEFINER keeps provider submission identity registration and
-- lease release in one fenced row-lock transition. Execution is restricted
-- to service_role and every relation is schema-qualified.
create function public.mark_msg_send_submission_waiting_result(
    p_msg_send_id bigint,
    p_lock_owner text,
    p_lease_token uuid,
    p_channel text,
    p_provider_request_id text,
    p_accepted_at timestamptz
)
returns table (
    applied boolean,
    duplicate boolean,
    message jsonb
)
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_message public.msg_send%rowtype;
    v_now timestamptz;
begin
    if p_msg_send_id is null or p_msg_send_id <= 0 then
        raise exception 'invalid message id'
            using errcode = '22023';
    end if;
    if p_lock_owner is null
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner))
          not between 1 and 128
       or p_lease_token is null then
        raise exception 'invalid claim fence'
            using errcode = '22023';
    end if;
    if p_channel is null or p_channel not in ('alimtalk', 'sms') then
        raise exception 'invalid submission channel'
            using errcode = '22023';
    end if;
    if p_provider_request_id is null
       or pg_catalog.length(p_provider_request_id) not between 1 and 256
       or p_accepted_at is null then
        raise exception 'invalid provider submission'
            using errcode = '22023';
    end if;

    v_now := pg_catalog.clock_timestamp();

    select m.*
    into strict v_message
    from public.msg_send as m
    where m.msg_send_id = p_msg_send_id
    for update;

    if v_message.status in (
        'waiting_result',
        'sent',
        'failed',
        'retry_wait',
        'processing',
        'accepted'
    )
       and (
           (
               p_channel = 'alimtalk'
               and v_message.provider_request_id
                   is not distinct from p_provider_request_id
               and v_message.provider_submission_lease_token
                   is not distinct from p_lease_token
           )
           or
           (
               p_channel = 'sms'
               and v_message.fallback_provider_request_id
                   is not distinct from p_provider_request_id
               and v_message.fallback_submission_lease_token
                   is not distinct from p_lease_token
           )
       ) then
        return query
        select
            false,
            true,
            pg_catalog.to_jsonb(v_message);
        return;
    end if;

    if v_message.status = 'waiting_result' then
        raise exception 'submission request mismatch'
            using errcode = '22000';
    end if;

    if v_message.status in (
        'sent',
        'failed',
        'suppressed',
        'cancelled'
    ) then
        raise exception 'late submission registration'
            using errcode = '55000';
    end if;

    if v_message.status is distinct from 'accepted'
       or v_message.lock_owner is distinct from p_lock_owner
       or v_message.lease_token is distinct from p_lease_token then
        raise exception 'stale submission start fence'
            using errcode = '55000';
    end if;
    if v_message.active_channel is distinct from p_channel then
        raise exception 'submission channel mismatch'
            using errcode = '22000';
    end if;
    if (
           p_channel = 'alimtalk'
           and (
               v_message.provider_submission_started_at is null
               or v_message.provider_submission_lease_token
                  is distinct from p_lease_token
           )
       )
       or (
           p_channel = 'sms'
           and (
               v_message.fallback_submission_started_at is null
               or v_message.fallback_submission_lease_token
                  is distinct from p_lease_token
           )
       ) then
        raise exception 'submission start identity mismatch'
            using errcode = '55000';
    end if;

    if p_channel = 'alimtalk' then
        update public.msg_send as m
        set
            status = 'waiting_result',
            provider_request_id = p_provider_request_id,
            provider_accepted_at = p_accepted_at,
            provider_submission_lease_token = p_lease_token,
            provider_result_id = null,
            provider_result_status = null,
            provider_result_retryable = null,
            provider_result_code = null,
            provider_result_at = null,
            provider_cost = null,
            lock_owner = null,
            lease_token = null,
            lease_expires_at = null,
            updated_at = v_now
        where m.msg_send_id = p_msg_send_id
        returning m.* into strict v_message;
    else
        update public.msg_send as m
        set
            status = 'waiting_result',
            fallback_provider = 'bizppurio',
            fallback_provider_request_id = p_provider_request_id,
            fallback_provider_accepted_at = p_accepted_at,
            fallback_submission_lease_token = p_lease_token,
            fallback_provider_result_id = null,
            fallback_provider_result_status = null,
            fallback_provider_result_retryable = null,
            fallback_provider_result_code = null,
            fallback_provider_result_at = null,
            fallback_provider_cost = null,
            lock_owner = null,
            lease_token = null,
            lease_expires_at = null,
            updated_at = v_now
        where m.msg_send_id = p_msg_send_id
        returning m.* into strict v_message;
    end if;

    return query
    select
        true,
        false,
        pg_catalog.to_jsonb(v_message);
end;
$function$;

revoke all on function public.mark_msg_send_submission_waiting_result(
    bigint,text,uuid,text,text,timestamptz
) from public, anon, authenticated;
grant execute on function public.mark_msg_send_submission_waiting_result(
    bigint,text,uuid,text,text,timestamptz
) to service_role;

-- SECURITY DEFINER provides the only claim completion path. Processing uses
-- the live lease deadline; a durable submission start uses its channel token
-- after the provider side effect may already have happened.
create function public.complete_msg_send_claim(
    p_msg_send_id bigint,
    p_lock_owner text,
    p_lease_token uuid,
    p_action text,
    p_available_at timestamptz,
    p_error_code text
)
returns jsonb
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_message public.msg_send%rowtype;
    v_now timestamptz;
begin
    if p_msg_send_id is null or p_msg_send_id <= 0 then
        raise exception 'invalid message id'
            using errcode = '22023';
    end if;
    if p_lock_owner is null
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner))
          not between 1 and 128
       or p_lease_token is null then
        raise exception 'invalid claim fence'
            using errcode = '22023';
    end if;
    if p_action is null
       or p_action not in (
           'retry_wait',
           'failed',
           'suppressed',
           'cancelled'
       ) then
        raise exception 'invalid claim completion action'
            using errcode = '22023';
    end if;
    if p_error_code is null
       or pg_catalog.length(p_error_code) not between 1 and 128 then
        raise exception 'invalid completion error code'
            using errcode = '22023';
    end if;

    v_now := pg_catalog.clock_timestamp();

    select m.*
    into strict v_message
    from public.msg_send as m
    where m.msg_send_id = p_msg_send_id
    for update;

    if v_message.lock_owner is distinct from p_lock_owner
       or v_message.lease_token is distinct from p_lease_token then
        raise exception 'stale or expired claim fence'
            using errcode = '55000';
    end if;
    if v_message.status = 'processing' then
        if v_message.lease_expires_at is null
           or v_message.lease_expires_at <= v_now then
            raise exception 'stale or expired claim fence'
                using errcode = '55000';
        end if;
    elsif v_message.status = 'accepted' then
        if p_action not in ('retry_wait', 'failed')
           or (
               v_message.active_channel = 'alimtalk'
               and (
                   v_message.provider_submission_started_at is null
                   or v_message.provider_submission_lease_token
                      is distinct from p_lease_token
               )
           )
           or (
               v_message.active_channel = 'sms'
               and (
                   v_message.fallback_submission_started_at is null
                   or v_message.fallback_submission_lease_token
                      is distinct from p_lease_token
               )
           ) then
            raise exception 'stale submission completion fence'
                using errcode = '55000';
        end if;
    else
        raise exception 'stale or expired claim fence'
            using errcode = '55000';
    end if;

    if p_action = 'retry_wait' then
        if p_available_at is null
           or p_available_at <= v_now
           or p_available_at >= v_message.expires_at then
            raise exception 'invalid retry schedule'
                using errcode = '22023';
        end if;
    elsif p_available_at is not null then
        raise exception 'terminal completion cannot set available_at'
            using errcode = '22023';
    end if;

    update public.msg_send as m
    set
        status = p_action,
        available_at = case
            when p_action = 'retry_wait' then p_available_at
            else m.available_at
        end,
        last_error_code = p_error_code,
        terminal_at = case
            when p_action in ('failed', 'suppressed', 'cancelled')
                then v_now
            else null
        end,
        lock_owner = null,
        lease_token = null,
        lease_expires_at = null,
        updated_at = v_now
    where m.msg_send_id = p_msg_send_id
    returning m.* into strict v_message;

    return pg_catalog.to_jsonb(v_message);
end;
$function$;

revoke all on function public.complete_msg_send_claim(
    bigint,text,uuid,text,timestamptz,text
) from public, anon, authenticated;
grant execute on function public.complete_msg_send_claim(
    bigint,text,uuid,text,timestamptz,text
) to service_role;

-- SECURITY DEFINER performs a bounded, non-blocking recovery pass before
-- normal claim. Interrupted processing may retry, but a durable provider
-- submission start is terminalized on its ambiguity/result deadline instead
-- of ever being reclaimed with a new provider correlation token.
create function public.recover_msg_send_leases(
    p_batch_size integer,
    p_max_attempts integer,
    p_retry_base_seconds integer,
    p_retry_max_seconds integer,
    p_waiting_result_timeout_seconds integer
)
returns integer
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_now timestamptz;
    v_recovered integer;
begin
    if p_batch_size is null or p_batch_size not between 1 and 1000 then
        raise exception 'recovery batch size must be between 1 and 1000'
            using errcode = '22023';
    end if;
    if p_max_attempts is null or p_max_attempts not between 1 and 10 then
        raise exception 'recovery max attempts must be between 1 and 10'
            using errcode = '22023';
    end if;
    if p_retry_base_seconds is null
       or p_retry_base_seconds not between 1 and 3600
       or p_retry_max_seconds is null
       or p_retry_max_seconds not between p_retry_base_seconds and 86400 then
        raise exception 'invalid recovery retry bounds'
            using errcode = '22023';
    end if;
    if p_waiting_result_timeout_seconds is null
       or p_waiting_result_timeout_seconds not between 60 and 86400 then
        raise exception
            'waiting result timeout must be between 60 and 86400'
            using errcode = '22023';
    end if;

    v_now := pg_catalog.clock_timestamp();

    with candidates as materialized (
        select
            m.msg_send_id,
            m.status as previous_status,
            case
                when m.status in ('pending', 'retry_wait')
                     and m.expires_at <= v_now
                    then 'queue_expired'
                when m.status = 'waiting_result'
                    then 'provider_result_timeout'
                when m.status = 'accepted'
                    then 'provider_submission_ambiguous_timeout'
                else 'lease_expired'
            end as recovery_reason,
            (
                case
                    when (
                        p_retry_base_seconds
                        * pg_catalog.power(
                            2::numeric,
                            (
                                case
                                    when m.attempt_count > 0
                                        then m.attempt_count - 1
                                    else 0
                                end
                            )::numeric
                        )
                    ) >= p_retry_max_seconds
                        then p_retry_max_seconds
                    else (
                        p_retry_base_seconds
                        * pg_catalog.power(
                            2::numeric,
                            (
                                case
                                    when m.attempt_count > 0
                                        then m.attempt_count - 1
                                    else 0
                                end
                            )::numeric
                        )
                    )::integer
                end
            ) as retry_delay_seconds,
            m.attempt_count >= (
                case
                    when m.max_attempts < p_max_attempts
                        then m.max_attempts
                    else p_max_attempts
                end
            ) as attempts_exhausted
        from public.msg_send as m
        where (
                m.status = 'processing'
                and m.lease_expires_at is not null
                and m.lease_expires_at <= v_now
            )
           or (
                m.status in ('pending', 'retry_wait')
                and m.expires_at <= v_now
            )
           or (
                m.status = 'accepted'
                and (
                    m.expires_at <= v_now
                    or (
                        m.active_channel = 'alimtalk'
                        and m.provider_submission_started_at
                            <= v_now - pg_catalog.make_interval(
                                secs =>
                                    p_waiting_result_timeout_seconds
                            )
                    )
                    or (
                        m.active_channel = 'sms'
                        and m.fallback_submission_started_at
                            <= v_now - pg_catalog.make_interval(
                                secs =>
                                    p_waiting_result_timeout_seconds
                            )
                    )
                )
            )
           or (
                m.status = 'waiting_result'
                and (
                    m.expires_at <= v_now
                    or (
                        case
                            when m.active_channel = 'alimtalk'
                                then m.provider_accepted_at
                            else m.fallback_provider_accepted_at
                        end
                    ) <= v_now - pg_catalog.make_interval(
                        secs => p_waiting_result_timeout_seconds
                    )
                )
            )
        order by
            m.updated_at,
            m.msg_send_id
        limit p_batch_size
        for update of m skip locked
    ),
    recovered as (
        update public.msg_send as m
        set
            status = case
                when c.recovery_reason in (
                    'queue_expired',
                    'provider_result_timeout',
                    'provider_submission_ambiguous_timeout'
                )
                    then 'failed'
                when c.attempts_exhausted
                     or v_now + pg_catalog.make_interval(
                         secs => c.retry_delay_seconds
                     ) >= m.expires_at
                    then 'failed'
                else 'retry_wait'
            end,
            available_at = case
                when c.recovery_reason in (
                    'queue_expired',
                    'provider_result_timeout',
                    'provider_submission_ambiguous_timeout'
                )
                    then m.available_at
                when c.attempts_exhausted
                     or v_now + pg_catalog.make_interval(
                         secs => c.retry_delay_seconds
                     ) >= m.expires_at
                    then m.available_at
                else v_now + pg_catalog.make_interval(
                    secs => c.retry_delay_seconds
                )
            end,
            last_error_code = case
                when c.recovery_reason = 'queue_expired'
                    then 'queue_expired'
                when c.recovery_reason = 'provider_result_timeout'
                    then 'provider_result_timeout'
                when c.recovery_reason =
                     'provider_submission_ambiguous_timeout'
                    then 'provider_submission_ambiguous_timeout'
                when c.attempts_exhausted
                     or v_now + pg_catalog.make_interval(
                         secs => c.retry_delay_seconds
                     ) >= m.expires_at
                    then 'lease_attempts_exhausted'
                else 'lease_expired'
            end,
            terminal_at = case
                when c.recovery_reason in (
                    'queue_expired',
                    'provider_result_timeout',
                    'provider_submission_ambiguous_timeout'
                )
                    then v_now
                when c.attempts_exhausted
                     or v_now + pg_catalog.make_interval(
                         secs => c.retry_delay_seconds
                     ) >= m.expires_at
                    then v_now
                else null
            end,
            lock_owner = null,
            lease_token = null,
            lease_expires_at = null,
            updated_at = v_now
        from candidates as c
        where m.msg_send_id = c.msg_send_id
        returning m.msg_send_id
    )
    select pg_catalog.count(*)::integer
    into v_recovered
    from recovered;

    return v_recovered;
end;
$function$;

revoke all on function public.recover_msg_send_leases(
    integer,integer,integer,integer,integer
) from public, anon, authenticated;
grant execute on function public.recover_msg_send_leases(
    integer,integer,integer,integer,integer
) to service_role;

-- SECURITY DEFINER keeps provider callback dedupe, retry/fallback choice,
-- and the immediate SMS fallback claim in one short row-lock transition.
-- A registered request/token pair or an unregistered durable start token is
-- the callback fence.
create function public.record_msg_send_push_result(
    p_provider_request_id text,
    p_submission_lease_token uuid,
    p_provider_result_id text,
    p_channel text,
    p_delivered boolean,
    p_retryable boolean,
    p_result_at timestamptz,
    p_result_code text,
    p_cost_amount numeric,
    p_lock_owner text,
    p_lease_seconds integer,
    p_max_attempts integer,
    p_retry_base_seconds integer,
    p_retry_max_seconds integer
)
returns table (
    applied boolean,
    duplicate boolean,
    action text,
    message jsonb
)
language plpgsql
volatile
security definer
set search_path = ''
as $function$
declare
    v_message public.msg_send%rowtype;
    v_policy public.message_policy%rowtype;
    v_now timestamptz;
    v_result_status text;
    v_result_code text;
    v_cost numeric(14, 4);
    v_effective_max_attempts integer;
    v_retry_delay_seconds integer;
begin
    if p_provider_request_id is null
       or pg_catalog.length(p_provider_request_id) not between 1 and 256
       or p_submission_lease_token is null
       or p_provider_result_id is null
       or pg_catalog.length(p_provider_result_id) not between 1 and 256 then
        raise exception 'invalid provider result identity'
            using errcode = '22023';
    end if;
    if p_channel is null or p_channel not in ('alimtalk', 'sms') then
        raise exception 'invalid result channel'
            using errcode = '22023';
    end if;
    if p_delivered is null or p_retryable is null then
        raise exception 'result delivery flags are required'
            using errcode = '22023';
    end if;
    if p_result_at is null
       or (
           p_result_code is not null
           and pg_catalog.length(p_result_code) not between 1 and 128
       ) then
        raise exception 'invalid provider result data'
            using errcode = '22023';
    end if;
    if p_cost_amount is null
       or p_cost_amount < 0
       or p_cost_amount > 9999999999.9999
       or p_cost_amount <> pg_catalog.round(p_cost_amount, 4) then
        raise exception 'provider result cost must fit numeric(14,4)'
            using errcode = '22003';
    end if;
    if p_lock_owner is null
       or pg_catalog.length(pg_catalog.btrim(p_lock_owner))
          not between 1 and 128 then
        raise exception 'invalid callback worker'
            using errcode = '22023';
    end if;
    if p_lease_seconds is null or p_lease_seconds not between 30 and 300 then
        raise exception 'fallback lease seconds must be between 30 and 300'
            using errcode = '22023';
    end if;
    if p_max_attempts is null or p_max_attempts not between 1 and 10 then
        raise exception 'callback max attempts must be between 1 and 10'
            using errcode = '22023';
    end if;
    if p_retry_base_seconds is null
       or p_retry_base_seconds not between 1 and 3600
       or p_retry_max_seconds is null
       or p_retry_max_seconds not between p_retry_base_seconds and 86400 then
        raise exception 'invalid callback retry bounds'
            using errcode = '22023';
    end if;

    v_now := pg_catalog.clock_timestamp();
    v_result_status := case
        when p_delivered then 'success'
        else 'failure'
    end;
    v_result_code := case
        when p_result_code is not null then p_result_code
        when p_delivered then 'delivered'
        else 'provider_delivery_failed'
    end;
    v_cost := p_cost_amount::numeric(14, 4);

    select m.*
    into v_message
    from public.msg_send as m
    where (
            p_channel = 'alimtalk'
            and m.provider_request_id = p_provider_request_id
            and m.provider_submission_lease_token
                = p_submission_lease_token
        )
       or (
            p_channel = 'sms'
            and m.fallback_provider_request_id = p_provider_request_id
            and m.fallback_submission_lease_token
                = p_submission_lease_token
        )
       or (
            m.status = 'accepted'
            and m.active_channel = p_channel
            and (
                (
                    p_channel = 'alimtalk'
                    and m.provider_submission_started_at is not null
                    and m.provider_submission_lease_token
                        = p_submission_lease_token
                )
                or
                (
                    p_channel = 'sms'
                    and m.fallback_submission_started_at is not null
                    and m.fallback_submission_lease_token
                        = p_submission_lease_token
                )
            )
        )
    for update;

    if not found then
        return query
        select false, false, 'unknown', null::jsonb;
        return;
    end if;

    -- Bizppurio may synchronously deliver a PUSH result before submit()
    -- returns its request id, or after the ordinary claim lease expires. The
    -- durable submission-start token is also the provider message_key, so
    -- register that identity under this row lock before applying the result.
    -- A later worker registration is an exact duplicate and cannot rewind it.
    if v_message.status = 'accepted' then
        if p_channel = 'alimtalk' then
            update public.msg_send as m
            set
                status = 'waiting_result',
                provider_request_id = p_provider_request_id,
                provider_accepted_at = p_result_at,
                provider_submission_lease_token =
                    p_submission_lease_token,
                provider_result_id = null,
                provider_result_status = null,
                provider_result_retryable = null,
                provider_result_code = null,
                provider_result_at = null,
                provider_cost = null,
                lock_owner = null,
                lease_token = null,
                lease_expires_at = null,
                updated_at = v_now
            where m.msg_send_id = v_message.msg_send_id
            returning m.* into strict v_message;
        else
            update public.msg_send as m
            set
                status = 'waiting_result',
                fallback_provider = 'bizppurio',
                fallback_provider_request_id =
                    p_provider_request_id,
                fallback_provider_accepted_at = p_result_at,
                fallback_submission_lease_token =
                    p_submission_lease_token,
                fallback_provider_result_id = null,
                fallback_provider_result_status = null,
                fallback_provider_result_retryable = null,
                fallback_provider_result_code = null,
                fallback_provider_result_at = null,
                fallback_provider_cost = null,
                lock_owner = null,
                lease_token = null,
                lease_expires_at = null,
                updated_at = v_now
            where m.msg_send_id = v_message.msg_send_id
            returning m.* into strict v_message;
        end if;
    end if;

    select p.*
    into strict v_policy
    from public.message_policy as p
    where p.message_policy_id = v_message.message_policy_id;

    if p_channel = 'alimtalk'
       and v_message.provider_result_id is not null then
        if v_message.provider_request_id
               is distinct from p_provider_request_id
           or v_message.provider_submission_lease_token
               is distinct from p_submission_lease_token
           or v_message.provider_result_id
               is distinct from p_provider_result_id
           or v_message.provider_result_status
               is distinct from v_result_status
           or v_message.provider_result_retryable
               is distinct from p_retryable
           or v_message.provider_result_code
               is distinct from v_result_code
           or v_message.provider_cost is distinct from v_cost then
            raise exception 'duplicate provider push result mismatch'
                using errcode = '22000';
        end if;
        return query
        select
            false,
            true,
            'duplicate',
            pg_catalog.to_jsonb(v_message);
        return;
    end if;

    if p_channel = 'sms'
       and v_message.fallback_provider_result_id is not null then
        if v_message.fallback_provider_request_id
               is distinct from p_provider_request_id
           or v_message.fallback_submission_lease_token
               is distinct from p_submission_lease_token
           or v_message.fallback_provider_result_id
               is distinct from p_provider_result_id
           or v_message.fallback_provider_result_status
               is distinct from v_result_status
           or v_message.fallback_provider_result_retryable
               is distinct from p_retryable
           or v_message.fallback_provider_result_code
               is distinct from v_result_code
           or v_message.fallback_provider_cost is distinct from v_cost then
            raise exception 'duplicate provider push result mismatch'
                using errcode = '22000';
        end if;
        return query
        select
            false,
            true,
            'duplicate',
            pg_catalog.to_jsonb(v_message);
        return;
    end if;

    if v_message.status is distinct from 'waiting_result'
       or v_message.active_channel is distinct from p_channel then
        raise exception 'message is not waiting for this provider result'
            using errcode = '55000';
    end if;

    v_effective_max_attempts := case
        when v_message.max_attempts < p_max_attempts
            then v_message.max_attempts
        else p_max_attempts
    end;
    v_retry_delay_seconds := case
        when (
            p_retry_base_seconds
            * pg_catalog.power(
                2::numeric,
                (
                    case
                        when v_message.attempt_count > 0
                            then v_message.attempt_count - 1
                        else 0
                    end
                )::numeric
            )
        ) >= p_retry_max_seconds
            then p_retry_max_seconds
        else (
            p_retry_base_seconds
            * pg_catalog.power(
                2::numeric,
                (
                    case
                        when v_message.attempt_count > 0
                            then v_message.attempt_count - 1
                        else 0
                    end
                )::numeric
            )
        )::integer
    end;

    if p_delivered then
        update public.msg_send as m
        set
            status = 'sent',
            provider_result_id = case
                when p_channel = 'alimtalk' then p_provider_result_id
                else m.provider_result_id
            end,
            provider_result_status = case
                when p_channel = 'alimtalk' then v_result_status
                else m.provider_result_status
            end,
            provider_result_retryable = case
                when p_channel = 'alimtalk' then p_retryable
                else m.provider_result_retryable
            end,
            provider_result_code = case
                when p_channel = 'alimtalk' then v_result_code
                else m.provider_result_code
            end,
            provider_result_at = case
                when p_channel = 'alimtalk' then p_result_at
                else m.provider_result_at
            end,
            provider_cost = case
                when p_channel = 'alimtalk' then v_cost
                else m.provider_cost
            end,
            fallback_provider_result_id = case
                when p_channel = 'sms' then p_provider_result_id
                else m.fallback_provider_result_id
            end,
            fallback_provider_result_status = case
                when p_channel = 'sms' then v_result_status
                else m.fallback_provider_result_status
            end,
            fallback_provider_result_retryable = case
                when p_channel = 'sms' then p_retryable
                else m.fallback_provider_result_retryable
            end,
            fallback_provider_result_code = case
                when p_channel = 'sms' then v_result_code
                else m.fallback_provider_result_code
            end,
            fallback_provider_result_at = case
                when p_channel = 'sms' then p_result_at
                else m.fallback_provider_result_at
            end,
            fallback_provider_cost = case
                when p_channel = 'sms' then v_cost
                else m.fallback_provider_cost
            end,
            delivered_channel = p_channel,
            cost_class = case
                when p_channel = 'alimtalk' then v_policy.primary_cost_class
                else v_policy.fallback_cost_class
            end,
            final_cost = case
                when p_channel = 'alimtalk' then v_cost
                else coalesce(m.provider_cost, 0) + v_cost
            end,
            last_error_code = null,
            sent_at = v_now,
            terminal_at = v_now,
            updated_at = v_now
        where m.msg_send_id = v_message.msg_send_id
        returning m.* into strict v_message;

        return query
        select
            true,
            false,
            'delivered',
            pg_catalog.to_jsonb(v_message);
        return;
    end if;

    if p_retryable
       and v_message.attempt_count < v_effective_max_attempts
       and (
           v_now + pg_catalog.make_interval(
               secs => v_retry_delay_seconds
           )
       ) < v_message.expires_at then
        update public.msg_send as m
        set
            status = 'retry_wait',
            provider_result_id = case
                when p_channel = 'alimtalk' then p_provider_result_id
                else m.provider_result_id
            end,
            provider_result_status = case
                when p_channel = 'alimtalk' then v_result_status
                else m.provider_result_status
            end,
            provider_result_retryable = case
                when p_channel = 'alimtalk' then p_retryable
                else m.provider_result_retryable
            end,
            provider_result_code = case
                when p_channel = 'alimtalk' then v_result_code
                else m.provider_result_code
            end,
            provider_result_at = case
                when p_channel = 'alimtalk' then p_result_at
                else m.provider_result_at
            end,
            provider_cost = case
                when p_channel = 'alimtalk' then v_cost
                else m.provider_cost
            end,
            fallback_provider_result_id = case
                when p_channel = 'sms' then p_provider_result_id
                else m.fallback_provider_result_id
            end,
            fallback_provider_result_status = case
                when p_channel = 'sms' then v_result_status
                else m.fallback_provider_result_status
            end,
            fallback_provider_result_retryable = case
                when p_channel = 'sms' then p_retryable
                else m.fallback_provider_result_retryable
            end,
            fallback_provider_result_code = case
                when p_channel = 'sms' then v_result_code
                else m.fallback_provider_result_code
            end,
            fallback_provider_result_at = case
                when p_channel = 'sms' then p_result_at
                else m.fallback_provider_result_at
            end,
            fallback_provider_cost = case
                when p_channel = 'sms' then v_cost
                else m.fallback_provider_cost
            end,
            available_at =
                v_now + pg_catalog.make_interval(
                    secs => v_retry_delay_seconds
                ),
            last_error_code = v_result_code,
            updated_at = v_now
        where m.msg_send_id = v_message.msg_send_id
        returning m.* into strict v_message;

        return query
        select
            true,
            false,
            'retry_scheduled',
            pg_catalog.to_jsonb(v_message);
        return;
    end if;

    if p_channel = 'alimtalk'
       and v_policy.allow_sms_fallback
       and v_policy.fallback_cost_class is not null
       and (
           v_now + pg_catalog.make_interval(secs => p_lease_seconds)
       ) < v_message.expires_at then
        update public.msg_send as m
        set
            status = 'processing',
            active_channel = 'sms',
            provider_result_id = p_provider_result_id,
            provider_result_status = v_result_status,
            provider_result_retryable = p_retryable,
            provider_result_code = v_result_code,
            provider_result_at = p_result_at,
            provider_cost = v_cost,
            fallback_provider = 'bizppurio',
            fallback_reason = v_result_code,
            available_at = v_now,
            lock_owner = p_lock_owner,
            lease_token = pg_catalog.gen_random_uuid(),
            lease_generation = m.lease_generation + 1,
            lease_expires_at =
                v_now + pg_catalog.make_interval(
                    secs => p_lease_seconds
                ),
            attempt_count = m.attempt_count + 1,
            last_attempt_at = v_now,
            last_error_code = v_result_code,
            updated_at = v_now
        where m.msg_send_id = v_message.msg_send_id
        returning m.* into strict v_message;

        return query
        select
            true,
            false,
            'sms_fallback',
            pg_catalog.to_jsonb(v_message);
        return;
    end if;

    update public.msg_send as m
    set
        status = 'failed',
        provider_result_id = case
            when p_channel = 'alimtalk' then p_provider_result_id
            else m.provider_result_id
        end,
        provider_result_status = case
            when p_channel = 'alimtalk' then v_result_status
            else m.provider_result_status
        end,
        provider_result_retryable = case
            when p_channel = 'alimtalk' then p_retryable
            else m.provider_result_retryable
        end,
        provider_result_code = case
            when p_channel = 'alimtalk' then v_result_code
            else m.provider_result_code
        end,
        provider_result_at = case
            when p_channel = 'alimtalk' then p_result_at
            else m.provider_result_at
        end,
        provider_cost = case
            when p_channel = 'alimtalk' then v_cost
            else m.provider_cost
        end,
        fallback_provider_result_id = case
            when p_channel = 'sms' then p_provider_result_id
            else m.fallback_provider_result_id
        end,
        fallback_provider_result_status = case
            when p_channel = 'sms' then v_result_status
            else m.fallback_provider_result_status
        end,
        fallback_provider_result_retryable = case
            when p_channel = 'sms' then p_retryable
            else m.fallback_provider_result_retryable
        end,
        fallback_provider_result_code = case
            when p_channel = 'sms' then v_result_code
            else m.fallback_provider_result_code
        end,
        fallback_provider_result_at = case
            when p_channel = 'sms' then p_result_at
            else m.fallback_provider_result_at
        end,
        fallback_provider_cost = case
            when p_channel = 'sms' then v_cost
            else m.fallback_provider_cost
        end,
        cost_class = case
            when p_channel = 'alimtalk' then v_policy.primary_cost_class
            else v_policy.fallback_cost_class
        end,
        final_cost = case
            when p_channel = 'alimtalk' then v_cost
            else coalesce(m.provider_cost, 0) + v_cost
        end,
        last_error_code = v_result_code,
        terminal_at = v_now,
        updated_at = v_now
    where m.msg_send_id = v_message.msg_send_id
    returning m.* into strict v_message;

    return query
    select
        true,
        false,
        'failed',
        pg_catalog.to_jsonb(v_message);
end;
$function$;

revoke all on function public.record_msg_send_push_result(
    text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,
    text,integer,integer,integer,integer
) from public, anon, authenticated;
grant execute on function public.record_msg_send_push_result(
    text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,
    text,integer,integer,integer,integer
) to service_role;

commit;
