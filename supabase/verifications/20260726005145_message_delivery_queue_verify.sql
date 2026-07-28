begin read only;

set local statement_timeout = '30s';

do $verify$
declare
    v_timestamptz_count integer;
begin
    if to_regclass('public.alert_contact') is null
       or to_regclass('public.message_policy') is null
       or to_regclass('public.msg_send') is null
       or to_regclass('public.message_link_token') is null then
        raise exception 'one or more message tables are missing';
    end if;

    if exists (
        select 1
        from (
            values
                ('alert_contact'),
                ('message_policy'),
                ('msg_send'),
                ('message_link_token')
        ) as expected(table_name)
        join pg_catalog.pg_class as c
          on c.oid = to_regclass('public.' || expected.table_name)
        where not c.relrowsecurity
    ) then
        raise exception 'one or more message tables lack RLS';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_policy as p
        where p.polrelid in (
            'public.alert_contact'::regclass,
            'public.message_policy'::regclass,
            'public.msg_send'::regclass,
            'public.message_link_token'::regclass
        )
    ) then
        raise exception 'unexpected message table RLS policy exists';
    end if;

    if exists (
        select 1
        from (
            values
                ('public.alert_contact', true),
                ('public.message_policy', true),
                ('public.msg_send', false),
                ('public.message_link_token', true)
        ) as expected(relation_name, require_update)
        where has_table_privilege(
                  'anon',
                  expected.relation_name,
                  'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER'
              )
           or has_table_privilege(
                  'authenticated',
                  expected.relation_name,
                  'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER'
              )
    ) then
        raise exception 'unexpected anon/authenticated message table privilege';
    end if;

    if exists (
        select 1
        from (
            values
                ('public.alert_contact', true),
                ('public.message_policy', true),
                ('public.msg_send', false),
                ('public.message_link_token', true)
        ) as expected(relation_name, require_update)
        where not has_table_privilege(
                  'service_role',
                  expected.relation_name,
                  'SELECT'
              )
           or not has_table_privilege(
                  'service_role',
                  expected.relation_name,
                  'INSERT'
              )
           or (
                  expected.require_update
                  and not has_table_privilege(
                      'service_role',
                      expected.relation_name,
                      'UPDATE'
                  )
              )
           or (
                  not expected.require_update
                  and has_table_privilege(
                      'service_role',
                      expected.relation_name,
                      'UPDATE'
                  )
              )
           or has_table_privilege(
                  'service_role',
                  expected.relation_name,
                  'DELETE'
              )
           or has_table_privilege(
                  'service_role',
                  expected.relation_name,
                  'TRUNCATE'
              )
           or has_table_privilege(
                  'service_role',
                  expected.relation_name,
                  'REFERENCES'
              )
           or has_table_privilege(
                  'service_role',
                  expected.relation_name,
                  'TRIGGER'
              )
    ) then
        raise exception 'service_role message table privilege mismatch';
    end if;

    if exists (
        select 1
        from (
            values
                ('public.alert_contact_alert_contact_id_seq'),
                ('public.message_policy_message_policy_id_seq'),
                ('public.msg_send_msg_send_id_seq'),
                ('public.message_link_token_message_link_token_id_seq')
        ) as expected(relation_name)
        where has_sequence_privilege(
                  'anon',
                  expected.relation_name,
                  'USAGE,SELECT,UPDATE'
              )
           or has_sequence_privilege(
                  'authenticated',
                  expected.relation_name,
                  'USAGE,SELECT,UPDATE'
              )
    ) then
        raise exception 'unexpected anon/authenticated message sequence privilege';
    end if;

    if exists (
        select 1
        from (
            values
                ('public.alert_contact_alert_contact_id_seq'),
                ('public.message_policy_message_policy_id_seq'),
                ('public.msg_send_msg_send_id_seq'),
                ('public.message_link_token_message_link_token_id_seq')
        ) as expected(relation_name)
        where not has_sequence_privilege(
                  'service_role',
                  expected.relation_name,
                  'USAGE'
              )
           or not has_sequence_privilege(
                  'service_role',
                  expected.relation_name,
                  'SELECT'
              )
           or has_sequence_privilege(
                  'service_role',
                  expected.relation_name,
                  'UPDATE'
              )
    ) then
        raise exception 'service_role message sequence privilege mismatch';
    end if;

    select count(*)
    into v_timestamptz_count
    from information_schema.columns as c
    where c.table_schema = 'public'
      and (
          (c.table_name = 'alert_contact'
           and c.column_name in (
               'consent_recorded_at',
               'consent_revoked_at',
               'created_at',
               'updated_at'
           ))
          or
          (c.table_name = 'message_policy'
           and c.column_name in (
               'effective_from',
               'effective_until',
               'created_at',
               'updated_at'
           ))
          or
          (c.table_name = 'msg_send'
           and c.column_name in (
               'available_at',
               'expires_at',
               'lease_expires_at',
               'last_attempt_at',
               'provider_submission_started_at',
               'provider_accepted_at',
               'provider_result_at',
               'fallback_submission_started_at',
               'fallback_provider_accepted_at',
               'fallback_provider_result_at',
               'sent_at',
               'terminal_at',
               'created_at',
               'updated_at'
           ))
          or
          (c.table_name = 'message_link_token'
           and c.column_name in (
               'expires_at',
               'consumed_at',
               'revoked_at',
               'created_at'
           ))
      )
      and c.data_type = 'timestamp with time zone';

    if v_timestamptz_count <> 26 then
        raise exception 'message timestamptz column contract mismatch: %',
            v_timestamptz_count;
    end if;

    if exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'message_link_token'
          and c.column_name in (
              'raw_token',
              'plain_token',
              'plaintext_token',
              'token_plaintext',
              'token_value',
              'opaque_token'
          )
    ) then
        raise exception 'unexpected plaintext token column';
    end if;

    if not exists (
        select 1
        from information_schema.columns as c
        where c.table_schema = 'public'
          and c.table_name = 'message_link_token'
          and c.column_name = 'token_hash'
          and c.data_type = 'bytea'
          and c.is_nullable = 'NO'
    ) then
        raise exception 'message link token hash column mismatch';
    end if;

    if exists (
        select required.constraint_name
        from (
            values
                ('msg_send_state_check'),
                ('msg_send_lease_state_check'),
                ('msg_send_terminal_state_check'),
                ('msg_send_provider_result_check'),
                ('msg_send_fallback_result_check'),
                ('msg_send_submission_state_check'),
                ('msg_send_channel_state_check'),
                ('msg_send_delivery_state_check'),
                ('msg_send_dedupe_key_unique'),
                ('msg_send_incident_template_stage_unique'),
                ('alert_contact_consent_state_check'),
                ('message_policy_retry_bounds_check'),
                ('message_policy_cost_check'),
                ('message_link_token_hash_length_check')
        ) as required(constraint_name)
        where not exists (
            select 1
            from pg_catalog.pg_constraint as c
            where c.conname = required.constraint_name
              and c.convalidated
              and c.conrelid in (
                  'public.alert_contact'::regclass,
                  'public.message_policy'::regclass,
                  'public.msg_send'::regclass,
                  'public.message_link_token'::regclass
              )
        )
    ) then
        raise exception 'one or more message constraints are missing or invalid';
    end if;

    if exists (
        select 1
        from (
            values
                ('msg_send_state_check', 'pending'),
                ('msg_send_state_check', 'waiting_result'),
                ('msg_send_state_check', 'cancelled'),
                ('msg_send_lease_state_check', 'processing'),
                ('msg_send_lease_state_check', 'lease_token'),
                ('msg_send_lease_state_check', 'lease_expires_at'),
                ('msg_send_terminal_state_check', 'sent'),
                ('msg_send_provider_result_check', 'provider_request_id'),
                ('msg_send_provider_result_check', 'provider_submission_started_at'),
                ('msg_send_provider_result_check', 'provider_submission_lease_token'),
                ('msg_send_provider_result_check', 'provider_result_retryable'),
                ('msg_send_fallback_result_check', 'fallback_provider_request_id'),
                ('msg_send_fallback_result_check', 'fallback_submission_started_at'),
                ('msg_send_fallback_result_check', 'fallback_submission_lease_token'),
                ('msg_send_fallback_result_check', 'fallback_provider_result_retryable'),
                ('msg_send_submission_state_check', 'waiting_result'),
                ('msg_send_submission_state_check', 'accepted'),
                ('msg_send_submission_state_check', 'provider_submission_started_at'),
                ('msg_send_submission_state_check', 'provider_submission_lease_token'),
                ('msg_send_channel_state_check', 'fallback_reason'),
                ('msg_send_channel_state_check', 'is distinct from'),
                ('msg_send_delivery_state_check', 'delivered_channel'),
                ('msg_send_delivery_state_check', 'success'),
                ('message_link_token_hash_length_check', 'octet_length'),
                ('message_link_token_hash_length_check', '32')
        ) as expected(constraint_name, required_fragment)
        where not exists (
            select 1
            from pg_catalog.pg_constraint as c
            where c.conname = expected.constraint_name
              and c.convalidated
              and c.conrelid in (
                  'public.msg_send'::regclass,
                  'public.message_link_token'::regclass
              )
              and pg_catalog.strpos(
                  pg_catalog.lower(
                      pg_catalog.pg_get_constraintdef(c.oid, true)
                  ),
                  expected.required_fragment
              ) > 0
        )
    ) then
        raise exception 'message constraint definition drift';
    end if;

    if to_regclass('public.idx_msg_send_claim') is null
       or to_regclass('public.idx_msg_send_lease') is null
       or to_regclass('public.uq_msg_send_provider_request') is null
       or to_regclass(
           'public.uq_msg_send_fallback_provider_request'
       ) is null
       or to_regclass('public.uq_msg_send_provider_result') is null
       or to_regclass('public.uq_msg_send_fallback_provider_result') is null
       or to_regclass('public.idx_message_link_token_expiry') is null then
        raise exception 'one or more message indexes are missing';
    end if;

    if to_regprocedure(
           'public.claim_msg_send(text,integer,integer)'
       ) is null
       or to_regprocedure(
           'public.claim_exact_one_shot_msg_send(bigint,text,integer)'
       ) is null
       or to_regprocedure(
           'public.mark_msg_send_submission_started(bigint,text,uuid,text)'
       ) is null
       or to_regprocedure(
           'public.mark_msg_send_submission_waiting_result(bigint,text,uuid,text,text,timestamptz)'
       ) is null
       or to_regprocedure(
           'public.complete_msg_send_claim(bigint,text,uuid,text,timestamptz,text)'
       ) is null
       or to_regprocedure(
           'public.recover_msg_send_leases(integer,integer,integer,integer,integer)'
       ) is null
       or to_regprocedure(
           'public.record_msg_send_push_result(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,text,integer,integer,integer,integer)'
       ) is null then
        raise exception 'one or more message functions are missing';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.claim_msg_send(text,integer,integer)'::regprocedure,
            'public.claim_exact_one_shot_msg_send(bigint,text,integer)'::regprocedure,
            'public.mark_msg_send_submission_started(bigint,text,uuid,text)'::regprocedure,
            'public.mark_msg_send_submission_waiting_result(bigint,text,uuid,text,text,timestamptz)'::regprocedure,
            'public.complete_msg_send_claim(bigint,text,uuid,text,timestamptz,text)'::regprocedure,
            'public.recover_msg_send_leases(integer,integer,integer,integer,integer)'::regprocedure,
            'public.record_msg_send_push_result(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,text,integer,integer,integer,integer)'::regprocedure
        )
          and (
              not p.prosecdef
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
        raise exception 'message function SECURITY DEFINER/search_path mismatch';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.claim_msg_send(text,integer,integer)'::regprocedure,
            'public.claim_exact_one_shot_msg_send(bigint,text,integer)'::regprocedure,
            'public.mark_msg_send_submission_started(bigint,text,uuid,text)'::regprocedure,
            'public.mark_msg_send_submission_waiting_result(bigint,text,uuid,text,text,timestamptz)'::regprocedure,
            'public.complete_msg_send_claim(bigint,text,uuid,text,timestamptz,text)'::regprocedure,
            'public.recover_msg_send_leases(integer,integer,integer,integer,integer)'::regprocedure,
            'public.record_msg_send_push_result(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,text,integer,integer,integer,integer)'::regprocedure
        )
          and (
              has_function_privilege('anon', p.oid, 'EXECUTE')
              or has_function_privilege('authenticated', p.oid, 'EXECUTE')
          )
    ) then
        raise exception 'unexpected anon/authenticated message function privilege';
    end if;

    if exists (
        select 1
        from pg_catalog.pg_proc as p
        where p.oid in (
            'public.claim_msg_send(text,integer,integer)'::regprocedure,
            'public.claim_exact_one_shot_msg_send(bigint,text,integer)'::regprocedure,
            'public.mark_msg_send_submission_started(bigint,text,uuid,text)'::regprocedure,
            'public.mark_msg_send_submission_waiting_result(bigint,text,uuid,text,text,timestamptz)'::regprocedure,
            'public.complete_msg_send_claim(bigint,text,uuid,text,timestamptz,text)'::regprocedure,
            'public.recover_msg_send_leases(integer,integer,integer,integer,integer)'::regprocedure,
            'public.record_msg_send_push_result(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,text,integer,integer,integer,integer)'::regprocedure
        )
          and not has_function_privilege(
              'service_role',
              p.oid,
              'EXECUTE'
          )
    ) then
        raise exception 'service_role lacks message function execute privilege';
    end if;
end;
$verify$;

select
    (select count(*) from public.alert_contact) as alert_contact_count,
    (select count(*) from public.message_policy) as message_policy_count,
    (select count(*) from public.msg_send) as msg_send_count,
    (select count(*) from public.message_link_token) as message_link_token_count;

rollback;
