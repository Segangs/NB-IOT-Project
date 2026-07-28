begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

do $command_wire_compact_payload$
declare
    v_response_oid regprocedure :=
        'public.publish_device_command_response(text,bigint,bigint,text)'::regprocedure;
    v_ack_oid regprocedure :=
        'public.publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text)'::regprocedure;
    v_definition text;
    v_rewritten text;
    v_owner_oid oid;
begin
    select pg_catalog.pg_get_functiondef(v_response_oid)
    into strict v_definition;

    if pg_catalog.strpos(
        v_definition,
        $new$v_payload := pg_catalog.replace(
        v_claim_receipt::text,
        ' ',
        ''
    );$new$
    ) = 0 then
        if pg_catalog.strpos(
            v_definition,
            $old$v_payload := pg_catalog.jsonb_build_array(
        v_receipt_request_id,
        v_receipt_cmd_id,
        v_receipt_opcode,
        v_receipt_job_id,
        v_receipt_ttl_seconds
    )::text;$old$
        ) = 0 then
            raise exception using
                errcode = '55000',
                message = 'command response compact payload source drift';
        end if;

        v_rewritten := pg_catalog.replace(
            v_definition,
            $old$v_payload := pg_catalog.jsonb_build_array(
        v_receipt_request_id,
        v_receipt_cmd_id,
        v_receipt_opcode,
        v_receipt_job_id,
        v_receipt_ttl_seconds
    )::text;$old$,
            $new$v_payload := pg_catalog.replace(
        v_claim_receipt::text,
        ' ',
        ''
    );$new$
        );
        if v_rewritten is not distinct from v_definition then
            raise exception using
                errcode = '55000',
                message = 'command response compact payload rewrite failed';
        end if;
        execute v_rewritten;
    end if;

    select pg_catalog.pg_get_functiondef(v_ack_oid)
    into strict v_definition;

    if pg_catalog.strpos(
        v_definition,
        $new$v_payload := pg_catalog.replace(
        v_ack_receipt::text,
        ' ',
        ''
    );$new$
    ) = 0 then
        if pg_catalog.strpos(
            v_definition,
            $old$v_payload := pg_catalog.jsonb_build_array(
        v_receipt_cmd_id,
        v_receipt_phase,
        v_receipt_result,
        v_receipt_status,
        v_receipt_error
    )::text;$old$
        ) = 0 then
            raise exception using
                errcode = '55000',
                message = 'command ACK compact payload source drift';
        end if;

        v_rewritten := pg_catalog.replace(
            v_definition,
            $old$v_payload := pg_catalog.jsonb_build_array(
        v_receipt_cmd_id,
        v_receipt_phase,
        v_receipt_result,
        v_receipt_status,
        v_receipt_error
    )::text;$old$,
            $new$v_payload := pg_catalog.replace(
        v_ack_receipt::text,
        ' ',
        ''
    );$new$
        );
        if v_rewritten is not distinct from v_definition then
            raise exception using
                errcode = '55000',
                message = 'command ACK compact payload rewrite failed';
        end if;
        execute v_rewritten;
    end if;

    select pg_catalog.pg_get_functiondef(v_response_oid)
    into strict v_definition;
    if pg_catalog.strpos(
        v_definition,
        $new$v_payload := pg_catalog.replace(
        v_claim_receipt::text,
        ' ',
        ''
    );$new$
    ) = 0 then
        raise exception using
            errcode = '55000',
            message = 'command response compact payload verification failed';
    end if;
    select p.proowner
    into strict v_owner_oid
    from pg_catalog.pg_proc as p
    where p.oid = v_response_oid;
    execute pg_catalog.format(
        'comment on function public.publish_device_command_response'
            || '(text,bigint,bigint,text) is %L',
        'Claims one command and queues its compact MQTT response. '
            || 'Returned bigint is a queued request ID only, '
            || 'not HTTP or MQTT delivery success;definition_md5='
            || pg_catalog.md5(v_definition)
            || ';owner_oid='
            || v_owner_oid::text
    );

    select pg_catalog.pg_get_functiondef(v_ack_oid)
    into strict v_definition;
    if pg_catalog.strpos(
        v_definition,
        $new$v_payload := pg_catalog.replace(
        v_ack_receipt::text,
        ' ',
        ''
    );$new$
    ) = 0 then
        raise exception using
            errcode = '55000',
            message = 'command ACK compact payload verification failed';
    end if;
    select p.proowner
    into strict v_owner_oid
    from pg_catalog.pg_proc as p
    where p.oid = v_ack_oid;
    execute pg_catalog.format(
        'comment on function public.publish_device_command_ack_receipt'
            || '(text,bigint,smallint,smallint,smallint,bigint,boolean,text) '
            || 'is %L',
        'Stores one ACK and queues its compact MQTT receipt. '
            || 'Returned bigint is a queued request ID only, '
            || 'not HTTP or MQTT delivery success;definition_md5='
            || pg_catalog.md5(v_definition)
            || ';owner_oid='
            || v_owner_oid::text
    );
end;
$command_wire_compact_payload$;

commit;
