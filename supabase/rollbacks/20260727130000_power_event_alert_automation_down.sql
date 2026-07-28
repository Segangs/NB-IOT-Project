begin;

set local lock_timeout = '5s';
set local statement_timeout = '30s';

do $rollback_guard$
begin
    if to_regclass('public.device_power_event') is not null
       and exists (select 1 from public.device_power_event) then
        raise exception
            'power event rows exist; explicit data decision required';
    end if;
end;
$rollback_guard$;

drop function if exists public.ingest_device_power_event(
    text,
    smallint,
    smallint,
    bigint,
    smallint,
    smallint,
    integer,
    integer,
    bigint,
    boolean,
    text
);
drop table if exists public.device_power_event;

delete from vault.secrets as d
where d.name = 'nb_iot_event_gateway_secret'
  and d.description =
      'NB-IOT power event gateway migration 20260727130000;secret_id='
      || d.id::text;

commit;
