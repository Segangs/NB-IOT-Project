#include "runtime_owner_rtos_drain_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using boot_v2::NormalIntent;
using boot_v2::NormalIntentKind;
using boot_v2::RuntimeOwnerActivationFacts;
using boot_v2::RuntimeOwnerActivationPreflightResult;
using boot_v2::RuntimeOwnerControlKind;
using boot_v2::RuntimeOwnerControlMessage;
using boot_v2::RuntimeOwnerDrainConsumeResult;
using boot_v2::RuntimeOwnerDrainResult;
using boot_v2::RuntimeOwnerDrainStep;
using boot_v2::RuntimeOwnerQueueReadResult;
using boot_v2::RuntimeOwnerRtosDrainMetrics;
using boot_v2::RuntimeOwnerRtosLane;
using boot_v2::RuntimeOwnerStopFacts;
using boot_v2::RuntimeOwnerStopPreflightResult;
using boot_v2::RuntimeOwnerShutdownIntent;
using boot_v2::RuntimeOwnerUrgentMessage;
using boot_v2::RuntimeOwnerUrgentSource;
using boot_v2::runtime_owner_activation_preflight;
using boot_v2::runtime_owner_drain_once;
using boot_v2::runtime_owner_drain_until_idle;
using boot_v2::runtime_owner_is_canonical_control;
using boot_v2::runtime_owner_is_canonical_urgent;
using boot_v2::runtime_owner_record_drain_step;
using boot_v2::runtime_owner_stop_preflight;

int checks = 0;
int failures = 0;

void check(const bool condition, const char *const expression, const int line)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Value, std::size_t Capacity>
struct FakeLane {
    std::array<Value, Capacity> values{};
    std::size_t size{0};
    std::size_t next{0};
    bool fault{false};

    bool push(const Value value) noexcept
    {
        if (size == Capacity) {
            return false;
        }
        values[size++] = value;
        return true;
    }

    RuntimeOwnerQueueReadResult receive(Value &output) noexcept
    {
        if (fault) {
            return RuntimeOwnerQueueReadResult::Fault;
        }
        if (next == size) {
            return RuntimeOwnerQueueReadResult::Empty;
        }
        output = values[next++];
        return RuntimeOwnerQueueReadResult::Received;
    }
};

struct FakeBackend {
    FakeLane<RuntimeOwnerUrgentMessage, 8> urgent{};
    FakeLane<RuntimeOwnerControlMessage, 8> control{};
    FakeLane<NormalIntent, 8> normal{};
    std::uint32_t urgent_reads{0};
    std::uint32_t control_reads{0};
    std::uint32_t normal_reads{0};

    RuntimeOwnerQueueReadResult receive_urgent(
        RuntimeOwnerUrgentMessage &output) noexcept
    {
        ++urgent_reads;
        return urgent.receive(output);
    }

    RuntimeOwnerQueueReadResult receive_control(
        RuntimeOwnerControlMessage &output) noexcept
    {
        ++control_reads;
        return control.receive(output);
    }

    RuntimeOwnerQueueReadResult receive_normal(NormalIntent &output) noexcept
    {
        ++normal_reads;
        return normal.receive(output);
    }
};

struct FakeSink {
    std::array<RuntimeOwnerRtosLane, 16> lanes{};
    std::array<std::uint32_t, 16> identities{};
    std::size_t calls{0};
    bool reject_normal{false};
    std::uint32_t direct_fallback_calls{0};

    RuntimeOwnerDrainConsumeResult consume_urgent(
        const RuntimeOwnerUrgentMessage input) noexcept
    {
        lanes[calls] = RuntimeOwnerRtosLane::Urgent;
        identities[calls++] = input.producer_sequence;
        return runtime_owner_is_canonical_urgent(input)
                   ? RuntimeOwnerDrainConsumeResult::Processed
                   : RuntimeOwnerDrainConsumeResult::DroppedInvalid;
    }

    RuntimeOwnerDrainConsumeResult consume_control(
        const RuntimeOwnerControlMessage input) noexcept
    {
        lanes[calls] = RuntimeOwnerRtosLane::Control;
        identities[calls++] = 0;
        return runtime_owner_is_canonical_control(input)
                   ? RuntimeOwnerDrainConsumeResult::Processed
                   : RuntimeOwnerDrainConsumeResult::DroppedInvalid;
    }

    RuntimeOwnerDrainConsumeResult consume_normal(
        const NormalIntent input) noexcept
    {
        lanes[calls] = RuntimeOwnerRtosLane::Normal;
        identities[calls++] = input.subject_id;
        return reject_normal
                   ? RuntimeOwnerDrainConsumeResult::DroppedInvalid
                   : RuntimeOwnerDrainConsumeResult::Processed;
    }
};

constexpr RuntimeOwnerUrgentMessage urgent_message(
    const std::uint32_t sequence) noexcept
{
    return {RuntimeOwnerUrgentSource::PowerButton,
            RuntimeOwnerShutdownIntent::AutomaticByUsb,
            {},
            sequence,
            91};
}

constexpr RuntimeOwnerControlMessage control_message() noexcept
{
    return {RuntimeOwnerControlKind::RequestTransportAttempt, {}};
}

constexpr NormalIntent normal_message(const std::uint32_t subject) noexcept
{
    NormalIntent value{};
    value.kind = NormalIntentKind::PublishTelemetry;
    value.flags = 0x01;
    value.value_deci_celsius =
        static_cast<std::int16_t>(subject);
    value.subject_id = subject;
    value.snapshot_revision = subject;
    return value;
}

struct RecordingRecorder {
    std::array<RuntimeOwnerDrainStep, 16> steps{};
    std::size_t calls{0};

    void operator()(const RuntimeOwnerDrainStep step) noexcept
    {
        steps[calls++] = step;
    }
};

struct UrgentInjectingSink : FakeSink {
    FakeBackend *backend{nullptr};

    RuntimeOwnerDrainConsumeResult consume_normal(
        const NormalIntent input) noexcept
    {
        const RuntimeOwnerDrainConsumeResult result =
            FakeSink::consume_normal(input);
        (void)backend->urgent.push(urgent_message(77));
        return result;
    }
};

constexpr bool step_is(
    const RuntimeOwnerDrainStep step,
    const RuntimeOwnerDrainResult result,
    const RuntimeOwnerRtosLane lane) noexcept
{
    return step.result == result && step.lane == lane &&
           step.reserved[0] == 0 && step.reserved[1] == 0;
}

std::string read_source(const char *const path)
{
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

std::size_t count_occurrences(
    const std::string &source,
    const std::string &needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string replace_once_copy(
    const std::string &source,
    const std::string &from,
    const std::string &to)
{
    std::string result = source;
    const std::size_t position = result.find(from);
    if (position != std::string::npos) {
        result.replace(position, from.size(), to);
    }
    return result;
}

struct SourceSpan {
    std::size_t begin{std::string::npos};
    std::size_t end{std::string::npos};

    [[nodiscard]] bool valid() const noexcept
    {
        return begin != std::string::npos &&
               end != std::string::npos &&
               begin < end;
    }
};

SourceSpan function_span(
    const std::string &source,
    const std::string &signature)
{
    const std::size_t begin = source.find(signature);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t opening = source.find('{', begin + signature.size());
    if (opening == std::string::npos) {
        return {};
    }

    enum class LexicalState {
        Code,
        StringLiteral,
        CharacterLiteral,
        LineComment,
        BlockComment,
    };

    std::size_t depth = 0;
    LexicalState state = LexicalState::Code;
    for (std::size_t position = opening; position < source.size();
         ++position) {
        const char current = source[position];
        const char next =
            position + 1 < source.size() ? source[position + 1] : '\0';
        switch (state) {
        case LexicalState::Code:
            if (current == '"') {
                state = LexicalState::StringLiteral;
            } else if (current == '\'') {
                state = LexicalState::CharacterLiteral;
            } else if (current == '/' && next == '/') {
                state = LexicalState::LineComment;
                ++position;
            } else if (current == '/' && next == '*') {
                state = LexicalState::BlockComment;
                ++position;
            } else if (current == '{') {
                ++depth;
            } else if (current == '}') {
                --depth;
                if (depth == 0) {
                    return {begin, position + 1};
                }
            }
            break;
        case LexicalState::StringLiteral:
            if (current == '\\' && next != '\0') {
                ++position;
            } else if (current == '"') {
                state = LexicalState::Code;
            }
            break;
        case LexicalState::CharacterLiteral:
            if (current == '\\' && next != '\0') {
                ++position;
            } else if (current == '\'') {
                state = LexicalState::Code;
            }
            break;
        case LexicalState::LineComment:
            if (current == '\n') {
                state = LexicalState::Code;
            }
            break;
        case LexicalState::BlockComment:
            if (current == '*' && next == '/') {
                state = LexicalState::Code;
                ++position;
            }
            break;
        }
    }
    return {};
}

std::string function_definition(
    const std::string &source,
    const std::string &signature)
{
    const SourceSpan span = function_span(source, signature);
    return span.valid()
               ? source.substr(span.begin, span.end - span.begin)
               : std::string{};
}

std::string move_function_before_copy(
    const std::string &source,
    const std::string &moved_signature,
    const std::string &anchor_signature)
{
    const SourceSpan moved = function_span(source, moved_signature);
    const SourceSpan anchor = function_span(source, anchor_signature);
    if (!moved.valid() || !anchor.valid() || moved.begin <= anchor.begin) {
        return source;
    }

    const std::string moved_definition =
        source.substr(moved.begin, moved.end - moved.begin);
    std::string result = source;
    result.erase(moved.begin, moved.end - moved.begin);
    result.insert(anchor.begin, moved_definition + "\n\n");
    return result;
}

bool critical_sections_exclude(
    const std::string &source,
    const std::string &needle)
{
    std::size_t search = 0;
    while (true) {
        const std::size_t enter =
            source.find("taskENTER_CRITICAL();", search);
        if (enter == std::string::npos) {
            return true;
        }
        const std::size_t exit =
            source.find("taskEXIT_CRITICAL();", enter);
        if (exit == std::string::npos) {
            return false;
        }
        const std::size_t forbidden = source.find(needle, enter);
        if (forbidden != std::string::npos && forbidden < exit) {
            return false;
        }
        search = exit + 1;
    }
}

bool redacted_status_cache_contract_accepts(const std::string &source)
{
    constexpr const char *publish_name =
        "publish_redacted_status_cache";
    constexpr const char *publish_call =
        "publish_redacted_status_cache();";
    const std::string publisher = function_definition(
        source, "void publish_redacted_status_cache() noexcept");
    const std::string drive = function_definition(
        source, "void drive_owner_until_idle() noexcept");
    const std::string shutdown_finalizer = function_definition(
        source,
        "[[noreturn]] void run_shutdown_finalizer(\n"
        "    const RuntimeOwnerUrgentMessage context) noexcept");
    const std::string task_entry = function_definition(
        source, "void runtime_owner_task_entry(void *) noexcept");
    const std::string activation = function_definition(
        source,
        "RuntimeOwnerAtomicCutoverResult "
        "runtime_owner_rtos_activate_atomic() noexcept");
    const std::string reader = function_definition(
        source,
        "RuntimeOwnerRedactedStatus runtime_owner_redacted_status() noexcept");
    if (publisher.empty() || drive.empty() || task_entry.empty() ||
        shutdown_finalizer.empty() || activation.empty() || reader.empty()) {
        return false;
    }

    const std::size_t snapshot =
        publisher.find("g_task_core.redacted_status()");
    const std::size_t publish_enter =
        publisher.find("taskENTER_CRITICAL();", snapshot);
    const std::size_t publish_copy =
        publisher.find("g_redacted_status_cache = snapshot;", publish_enter);
    const std::size_t publish_exit =
        publisher.find("taskEXIT_CRITICAL();", publish_copy);

    const std::size_t reader_enter =
        reader.find("taskENTER_CRITICAL();");
    const std::size_t reader_copy =
        reader.find("status = g_redacted_status_cache;", reader_enter);
    const std::size_t reader_exit =
        reader.find("taskEXIT_CRITICAL();", reader_copy);
    const std::size_t reader_return =
        reader.find("return status;", reader_exit);

    const std::size_t activation_guard =
        activation.find("if (activation !=");
    const std::size_t activation_failure_return = activation.find(
        "return RuntimeOwnerAtomicCutoverResult::RejectedNotReady;",
        activation_guard);
    const std::size_t activation_publish =
        activation.find(publish_call, activation_failure_return);
    const std::size_t activation_commit =
        activation.find("g_cutover_core.commit(", activation_publish);

    const std::size_t drain =
        task_entry.find("runtime_owner_drain_once(");
    const std::size_t drain_publish =
        task_entry.find(publish_call, drain);
    const std::size_t drain_metrics =
        task_entry.find("runtime_owner_record_drain_step(", drain_publish);
    const std::size_t drive_call =
        task_entry.find("drive_owner_until_idle();");
    const std::size_t drive_publish =
        task_entry.find(publish_call, drive_call);
    const std::size_t shutdown_guard =
        task_entry.find("if (shutdown_received)", drive_publish);
    const std::size_t final_publish =
        task_entry.find(publish_call, shutdown_guard);
    const std::size_t finalizer_call =
        task_entry.find("run_shutdown_finalizer(shutdown_context);",
                        final_publish);

    return source.find("RuntimeOwnerRedactedStatus "
                       "g_redacted_status_cache{};") != std::string::npos &&
           count_occurrences(
               source, "g_task_core.redacted_status()") == 1 &&
           count_occurrences(
               source, "g_redacted_status_cache =") == 1 &&
           count_occurrences(source, publish_call) == 4 &&
           count_occurrences(publisher, publish_name) == 1 &&
           snapshot != std::string::npos &&
           publish_enter != std::string::npos &&
           publish_copy != std::string::npos &&
           publish_exit != std::string::npos &&
           snapshot < publish_enter &&
           publish_enter < publish_copy &&
           publish_copy < publish_exit &&
           publisher.find("g_device_backend") == std::string::npos &&
           publisher.find("g_owner_loop") == std::string::npos &&
           publisher.find("modem") == std::string::npos &&
           reader.find("g_task_core") == std::string::npos &&
           count_occurrences(reader, "taskENTER_CRITICAL();") == 1 &&
           count_occurrences(reader, "taskEXIT_CRITICAL();") == 1 &&
           count_occurrences(
               reader, "status = g_redacted_status_cache;") == 1 &&
           reader_enter != std::string::npos &&
           reader_copy != std::string::npos &&
           reader_exit != std::string::npos &&
           reader_return != std::string::npos &&
           reader_enter < reader_copy &&
           reader_copy < reader_exit &&
           reader_exit < reader_return &&
           activation_guard != std::string::npos &&
           activation_failure_return != std::string::npos &&
           activation_publish != std::string::npos &&
           activation_commit != std::string::npos &&
           activation_guard < activation_failure_return &&
           activation_failure_return < activation_publish &&
           activation_publish < activation_commit &&
           drain != std::string::npos &&
           drain_publish != std::string::npos &&
           drain_metrics != std::string::npos &&
           drain < drain_publish &&
           drain_publish < drain_metrics &&
           drive_call != std::string::npos &&
           drive_publish != std::string::npos &&
           shutdown_guard != std::string::npos &&
           final_publish != std::string::npos &&
           finalizer_call != std::string::npos &&
           drive_call < drive_publish &&
           drive_publish < shutdown_guard &&
           shutdown_guard < final_publish &&
           final_publish < finalizer_call &&
           critical_sections_exclude(drive, "g_owner_loop.advance()") &&
           critical_sections_exclude(
               drive, "g_owner_loop.submit_deferred_config(") &&
           critical_sections_exclude(
               drive, "g_owner_loop.execute_one(") &&
           critical_sections_exclude(
               shutdown_finalizer,
               "g_owner_loop.execute_shutdown_cleanup(") &&
           critical_sections_exclude(
               task_entry, "run_shutdown_finalizer(") &&
           count_occurrences(task_entry, publish_call) == 3 &&
           count_occurrences(activation, publish_call) == 1;
}

bool test_redacted_status_cache_contract_and_publish_point_mutants()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    if (!redacted_status_cache_contract_accepts(source)) {
        return false;
    }

    const std::string direct_core_reader = replace_once_copy(
        source,
        "status = g_redacted_status_cache;",
        "status = g_task_core.redacted_status();");
    const std::string extra_reader_producer = replace_once_copy(
        source,
        "    RuntimeOwnerRedactedStatus status{};\n"
        "    taskENTER_CRITICAL();\n",
        "    RuntimeOwnerRedactedStatus status{};\n"
        "    g_redacted_status_cache = status;\n"
        "    taskENTER_CRITICAL();\n");
    const std::string missing_activation_publish = replace_once_copy(
        source,
        "    publish_redacted_status_cache();\n"
        "    if (g_cutover_core.commit(",
        "    if (g_cutover_core.commit(");
    const std::string missing_drain_publish = replace_once_copy(
        source,
        "                g_queue_backend, g_owner_loop);\n"
        "            if (g_owner_loop."
        "take_alarm_delivery_overflow_log_pending()) {\n"
        "                LOG(\"ALARM_DELIVERY_OVERFLOW\\n\");\n"
        "            }\n"
        "            publish_redacted_status_cache();\n",
        "                g_queue_backend, g_owner_loop);\n"
        "            if (g_owner_loop."
        "take_alarm_delivery_overflow_log_pending()) {\n"
        "                LOG(\"ALARM_DELIVERY_OVERFLOW\\n\");\n"
        "            }\n");
    const std::string missing_drive_publish = replace_once_copy(
        source,
        "        drive_owner_until_idle();\n"
        "        publish_redacted_status_cache();\n",
        "        drive_owner_until_idle();\n");
    const std::string missing_final_publish = replace_once_copy(
        source,
        "        if (shutdown_received) {\n"
        "            publish_redacted_status_cache();\n"
        "            run_shutdown_finalizer(shutdown_context);\n",
        "        if (shutdown_received) {\n"
        "            run_shutdown_finalizer(shutdown_context);\n");
    const std::string physical_under_critical = replace_once_copy(
        source,
        "        const RuntimeOwnerPhysicalStepResult physical =\n"
        "            g_owner_loop.execute_one(g_device_backend);\n",
        "        taskENTER_CRITICAL();\n"
        "        const RuntimeOwnerPhysicalStepResult physical =\n"
        "            g_owner_loop.execute_one(g_device_backend);\n"
        "        taskEXIT_CRITICAL();\n");
    const std::string harmless_helper_reorder = move_function_before_copy(
        source,
        "void increment_saturating(std::uint32_t &counter) noexcept",
        "void publish_redacted_status_cache() noexcept");
    const std::string cleanup_under_critical = replace_once_copy(
        source,
        "            const RuntimeOwnerShutdownStepResult result =\n"
        "                g_owner_loop.execute_shutdown_cleanup(\n"
        "                    g_device_backend,\n"
        "                    directive,\n"
        "                    context);\n",
        "            taskENTER_CRITICAL();\n"
        "            const RuntimeOwnerShutdownStepResult result =\n"
        "                g_owner_loop.execute_shutdown_cleanup(\n"
        "                    g_device_backend,\n"
        "                    directive,\n"
        "                    context);\n"
        "            taskEXIT_CRITICAL();\n");
    const std::string finalizer_under_critical = replace_once_copy(
        source,
        "            publish_redacted_status_cache();\n"
        "            run_shutdown_finalizer(shutdown_context);\n",
        "            publish_redacted_status_cache();\n"
        "            taskENTER_CRITICAL();\n"
        "            run_shutdown_finalizer(shutdown_context);\n"
        "            taskEXIT_CRITICAL();\n");

    return direct_core_reader != source &&
           extra_reader_producer != source &&
           missing_activation_publish != source &&
           missing_drain_publish != source &&
           missing_drive_publish != source &&
           missing_final_publish != source &&
           physical_under_critical != source &&
           harmless_helper_reorder != source &&
           cleanup_under_critical != source &&
           finalizer_under_critical != source &&
           redacted_status_cache_contract_accepts(
               harmless_helper_reorder) &&
           !redacted_status_cache_contract_accepts(direct_core_reader) &&
           !redacted_status_cache_contract_accepts(extra_reader_producer) &&
           !redacted_status_cache_contract_accepts(
               missing_activation_publish) &&
           !redacted_status_cache_contract_accepts(missing_drain_publish) &&
           !redacted_status_cache_contract_accepts(missing_drive_publish) &&
           !redacted_status_cache_contract_accepts(missing_final_publish) &&
           !redacted_status_cache_contract_accepts(physical_under_critical) &&
           !redacted_status_cache_contract_accepts(cleanup_under_critical) &&
           !redacted_status_cache_contract_accepts(
               finalizer_under_critical);
}

bool test_balanced_function_scanner_ignores_lexical_braces()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    constexpr const char *publisher_opening =
        "void publish_redacted_status_cache() noexcept\n"
        "{\n";
    const std::string string_literal_brace = replace_once_copy(
        source,
        publisher_opening,
        std::string(publisher_opening) + "    LOG(\"}\");\n");
    const std::string character_literal_brace = replace_once_copy(
        source,
        publisher_opening,
        std::string(publisher_opening) +
            "    const char harmless_brace = '}';\n"
            "    (void)harmless_brace;\n");
    const std::string line_comment_brace = replace_once_copy(
        source,
        publisher_opening,
        std::string(publisher_opening) + "    // }\n");
    const std::string block_comment_brace = replace_once_copy(
        source,
        publisher_opening,
        std::string(publisher_opening) + "    /* } */\n");

    return string_literal_brace != source &&
           character_literal_brace != source &&
           line_comment_brace != source &&
           block_comment_brace != source &&
           redacted_status_cache_contract_accepts(string_literal_brace) &&
           redacted_status_cache_contract_accepts(
               character_literal_brace) &&
           redacted_status_cache_contract_accepts(line_comment_brace) &&
           redacted_status_cache_contract_accepts(block_comment_brace);
}

bool test_rtos_source_includes_and_calls_shared_drain()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    return !source.empty() &&
           source.find("runtime_owner_rtos_drain_core.hpp") !=
               std::string::npos &&
           count_occurrences(
               source, "runtime_owner_drain_until_idle(") == 0 &&
           count_occurrences(source, "runtime_owner_drain_once(") == 1 &&
           source.find("copy_shutdown_context(shutdown_context)") !=
               std::string::npos &&
           source.find("SHUTDOWN_INGRESS_CLOSED") !=
               std::string::npos &&
           source.find("runtime_owner_record_drain_step") !=
               std::string::npos;
}

bool test_urgent_queue_uses_provenance_message()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    return source.find("alignas(RuntimeOwnerUrgentMessage)") !=
               std::string::npos &&
           source.find(
               "kUrgentDepth * sizeof(RuntimeOwnerUrgentMessage)") !=
               std::string::npos &&
           count_occurrences(
               source, "sizeof(RuntimeOwnerUrgentMessage)") == 2;
}

bool test_public_surface_remains_read_only_and_narrow()
{
    const std::string header = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.hpp");
    return header.find("runtime_owner_rtos_drain_metrics") !=
               std::string::npos &&
           header.find("try_submit_urgent") == std::string::npos &&
           header.find("try_request_shutdown") == std::string::npos &&
           header.find("runtime_owner_rtos_activate_atomic") !=
               std::string::npos &&
           header.find("runtime_owner_activate") == std::string::npos &&
           header.find("runtime_owner_deactivate") == std::string::npos &&
           header.find("set_ingress") == std::string::npos &&
           header.find("QueueHandle_t") == std::string::npos &&
           header.find("TaskHandle_t") == std::string::npos;
}

bool test_production_atomic_cutover_is_single_and_ordered()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    const std::size_t ready =
        source.find("g_drain_metrics.cutover_ready = 1");
    const std::size_t ingress =
        source.find("g_status.ingress_enabled = 1");
    return ingress != std::string::npos &&
           source.find("g_status.ingress_enabled = 0") !=
               std::string::npos &&
           source.find("g_drain_metrics.receiver_ready = 1") !=
               std::string::npos &&
           source.find("g_drain_metrics.cutover_ready = 0") !=
               std::string::npos &&
           ready != std::string::npos && ready < ingress &&
           source.find("g_cutover_coordinator.activate(") !=
               std::string::npos;
}

bool test_numeric_layout_and_default_contracts()
{
    const RuntimeOwnerRtosDrainMetrics metrics{};
    return static_cast<std::uint8_t>(RuntimeOwnerRtosLane::None) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerRtosLane::Urgent) == 1 &&
           static_cast<std::uint8_t>(RuntimeOwnerRtosLane::Control) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerRtosLane::Normal) == 3 &&
           static_cast<std::uint8_t>(RuntimeOwnerQueueReadResult::Empty) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerQueueReadResult::Received) == 1 &&
           static_cast<std::uint8_t>(RuntimeOwnerQueueReadResult::Fault) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerDrainResult::Empty) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerDrainResult::Processed) == 1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerDrainResult::DroppedInvalid) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerDrainResult::Fault) == 3 &&
           std::is_same<
               std::underlying_type<RuntimeOwnerDrainConsumeResult>::type,
               std::uint8_t>::value &&
           static_cast<std::uint8_t>(
               RuntimeOwnerDrainConsumeResult::Processed) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerDrainConsumeResult::DroppedInvalid) == 1 &&
           std::is_same<
               std::underlying_type<RuntimeOwnerActivationPreflightResult>::
                   type,
               std::uint8_t>::value &&
           static_cast<std::uint8_t>(
               RuntimeOwnerActivationPreflightResult::RejectedInvalid) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerActivationPreflightResult::RejectedIncomplete) ==
               1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerActivationPreflightResult::Ready) == 2 &&
           std::is_same<
               std::underlying_type<RuntimeOwnerStopPreflightResult>::type,
               std::uint8_t>::value &&
           static_cast<std::uint8_t>(
               RuntimeOwnerStopPreflightResult::RejectedInvalid) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerStopPreflightResult::SafePreAdmissionAbort) == 1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerUrgentSource::Invalid) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerUrgentSource::PowerButton) == 1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerUrgentSource::AdapterLossCommitted) == 2 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand) == 3 &&
           static_cast<std::uint8_t>(RuntimeOwnerShutdownIntent::Invalid) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerShutdownIntent::AutomaticByUsb) == 1 &&
           static_cast<std::uint8_t>(RuntimeOwnerShutdownIntent::Reboot) == 2 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerShutdownIntent::PowerOff) == 3 &&
           sizeof(RuntimeOwnerUrgentMessage) == 12 &&
           alignof(RuntimeOwnerUrgentMessage) == 4 &&
           sizeof(RuntimeOwnerDrainStep) == 4 &&
           alignof(RuntimeOwnerDrainStep) == 1 &&
           sizeof(RuntimeOwnerActivationFacts) == 8 &&
           alignof(RuntimeOwnerActivationFacts) == 1 &&
           sizeof(RuntimeOwnerStopFacts) == 8 &&
           alignof(RuntimeOwnerStopFacts) == 1 &&
           sizeof(RuntimeOwnerRtosDrainMetrics) == 24 &&
           alignof(RuntimeOwnerRtosDrainMetrics) == 4 &&
           std::is_standard_layout<RuntimeOwnerUrgentMessage>::value &&
           std::is_trivially_copyable<RuntimeOwnerUrgentMessage>::value &&
           metrics.urgent_processed_count == 0 &&
           metrics.control_processed_count == 0 &&
           metrics.normal_processed_count == 0 &&
           metrics.dropped_invalid_count == 0 &&
           metrics.receive_fault_count == 0 && metrics.receiver_ready == 0 &&
           metrics.cutover_ready == 0 && metrics.reserved[0] == 0 &&
           metrics.reserved[1] == 0;
}

bool test_all_empty_reads_no_sink()
{
    FakeBackend backend{};
    FakeSink sink{};
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 1 && sink.calls == 0;
}

bool test_urgent_precedes_control_and_normal()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.urgent.push(urgent_message(1));
    (void)backend.control.push(control_message());
    (void)backend.normal.push(normal_message(3));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           backend.urgent_reads == 1 && backend.control_reads == 0 &&
           backend.normal_reads == 0 && sink.calls == 1 &&
           sink.lanes[0] == RuntimeOwnerRtosLane::Urgent;
}

bool test_control_precedes_normal()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.control.push(control_message());
    (void)backend.normal.push(normal_message(4));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Control) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 0 && sink.calls == 1;
}

bool test_normal_only()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.normal.push(normal_message(41));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Normal) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 1 && sink.calls == 1 &&
           sink.identities[0] == 41;
}

bool test_next_pass_rechecks_new_urgent()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.normal.push(normal_message(51));
    const auto first = runtime_owner_drain_once(backend, sink);
    (void)backend.urgent.push(urgent_message(52));
    const auto second = runtime_owner_drain_once(backend, sink);
    return step_is(first, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Normal) &&
           step_is(second, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           sink.calls == 2 &&
           sink.lanes[0] == RuntimeOwnerRtosLane::Normal &&
           sink.lanes[1] == RuntimeOwnerRtosLane::Urgent;
}

bool test_drain_until_idle_records_once_and_rechecks_urgent()
{
    FakeBackend backend{};
    UrgentInjectingSink sink{};
    sink.backend = &backend;
    RecordingRecorder recorder{};
    (void)backend.normal.push(normal_message(51));

    const RuntimeOwnerDrainStep terminal =
        runtime_owner_drain_until_idle(backend, sink, recorder);

    return step_is(terminal,
                   RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           recorder.calls == 3 &&
           step_is(recorder.steps[0],
                   RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Normal) &&
           step_is(recorder.steps[1],
                   RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           step_is(recorder.steps[2],
                   RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           sink.calls == 2;
}

bool test_drain_until_idle_records_fault_once_and_stops()
{
    FakeBackend backend{};
    FakeSink sink{};
    RecordingRecorder recorder{};
    backend.control.fault = true;

    const RuntimeOwnerDrainStep terminal =
        runtime_owner_drain_until_idle(backend, sink, recorder);

    return step_is(terminal,
                   RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Control) &&
           recorder.calls == 1 &&
           step_is(recorder.steps[0],
                   RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Control) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 0 && sink.calls == 0;
}

bool test_drain_until_idle_metrics_count_drop_and_terminal_empty_once()
{
    FakeBackend backend{};
    FakeSink sink{};
    sink.reject_normal = true;
    RuntimeOwnerRtosDrainMetrics metrics{};
    std::size_t recorder_calls = 0;
    (void)backend.normal.push(normal_message(61));

    const RuntimeOwnerDrainStep terminal = runtime_owner_drain_until_idle(
        backend,
        sink,
        [&metrics, &recorder_calls](const RuntimeOwnerDrainStep step) noexcept {
            ++recorder_calls;
            runtime_owner_record_drain_step(metrics, step);
        });

    return step_is(terminal,
                   RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           recorder_calls == 2 && metrics.dropped_invalid_count == 1 &&
           metrics.normal_processed_count == 0 &&
           metrics.receive_fault_count == 0;
}

bool test_fifo_within_lane()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.urgent.push(urgent_message(7));
    (void)backend.urgent.push(urgent_message(8));
    const auto first = runtime_owner_drain_once(backend, sink);
    const auto second = runtime_owner_drain_once(backend, sink);
    return step_is(first, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           step_is(second, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           sink.calls == 2 && sink.identities[0] == 7 &&
           sink.identities[1] == 8;
}

bool test_urgent_fault_stops_lower_reads()
{
    FakeBackend backend{};
    FakeSink sink{};
    backend.urgent.fault = true;
    (void)backend.control.push(control_message());
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Urgent) &&
           backend.control_reads == 0 && backend.normal_reads == 0 &&
           sink.calls == 0;
}

bool test_control_fault_stops_normal_read()
{
    FakeBackend backend{};
    FakeSink sink{};
    backend.control.fault = true;
    (void)backend.normal.push(normal_message(5));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Control) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 0 && sink.calls == 0;
}

bool test_normal_fault_calls_no_sink()
{
    FakeBackend backend{};
    FakeSink sink{};
    backend.normal.fault = true;
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Normal) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 1 && sink.calls == 0;
}

bool urgent_drops(const RuntimeOwnerUrgentMessage input)
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.urgent.push(input);
    return step_is(runtime_owner_drain_once(backend, sink),
                   RuntimeOwnerDrainResult::DroppedInvalid,
                   RuntimeOwnerRtosLane::Urgent) &&
           sink.calls == 1;
}

bool test_invalid_urgent_variants_drop()
{
    auto source = urgent_message(1);
    source.source = RuntimeOwnerUrgentSource::Invalid;
    auto intent = urgent_message(1);
    intent.intent = RuntimeOwnerShutdownIntent::Invalid;
    auto local_reboot = urgent_message(1);
    local_reboot.intent = RuntimeOwnerShutdownIntent::Reboot;
    auto authenticated_automatic = urgent_message(1);
    authenticated_automatic.source =
        RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand;
    auto sequence = urgent_message(1);
    sequence.producer_sequence = 0;
    auto correlation = urgent_message(1);
    correlation.incident_correlation_id = 0;
    auto reserved0 = urgent_message(1);
    reserved0.reserved[0] = 1;
    auto reserved1 = urgent_message(1);
    reserved1.reserved[1] = 1;
    return urgent_drops(source) && urgent_drops(intent) &&
           urgent_drops(local_reboot) &&
           urgent_drops(authenticated_automatic) &&
           urgent_drops(sequence) &&
           urgent_drops(correlation) && urgent_drops(reserved0) &&
           urgent_drops(reserved1);
}

bool control_drops(const RuntimeOwnerControlMessage input)
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.control.push(input);
    return step_is(runtime_owner_drain_once(backend, sink),
                   RuntimeOwnerDrainResult::DroppedInvalid,
                   RuntimeOwnerRtosLane::Control) &&
           sink.calls == 1;
}

bool test_invalid_control_variants_drop()
{
    RuntimeOwnerControlMessage kind{};
    auto reserved0 = control_message();
    reserved0.reserved[0] = 1;
    auto reserved1 = control_message();
    reserved1.reserved[1] = 1;
    auto reserved2 = control_message();
    reserved2.reserved[2] = 1;
    return control_drops(kind) && control_drops(reserved0) &&
           control_drops(reserved1) && control_drops(reserved2);
}

bool test_normal_rejection_has_no_fallback()
{
    FakeBackend backend{};
    FakeSink sink{};
    sink.reject_normal = true;
    (void)backend.normal.push(normal_message(61));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::DroppedInvalid,
                   RuntimeOwnerRtosLane::Normal) &&
           sink.calls == 1 && sink.direct_fallback_calls == 0;
}

bool metrics_equal(
    const RuntimeOwnerRtosDrainMetrics left,
    const RuntimeOwnerRtosDrainMetrics right) noexcept
{
    return left.urgent_processed_count == right.urgent_processed_count &&
           left.control_processed_count == right.control_processed_count &&
           left.normal_processed_count == right.normal_processed_count &&
           left.dropped_invalid_count == right.dropped_invalid_count &&
           left.receive_fault_count == right.receive_fault_count &&
           left.receiver_ready == right.receiver_ready &&
           left.cutover_ready == right.cutover_ready &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1];
}

bool test_metrics_exact_increment()
{
    RuntimeOwnerRtosDrainMetrics metrics{};
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Urgent, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Control, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Normal, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::DroppedInvalid,
                  RuntimeOwnerRtosLane::Urgent, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Fault,
                  RuntimeOwnerRtosLane::Control, {}});
    const RuntimeOwnerRtosDrainMetrics before_empty = metrics;
    runtime_owner_record_drain_step(metrics, {});
    return metrics.urgent_processed_count == 1 &&
           metrics.control_processed_count == 1 &&
           metrics.normal_processed_count == 1 &&
           metrics.dropped_invalid_count == 1 &&
           metrics.receive_fault_count == 1 &&
           metrics_equal(metrics, before_empty);
}

bool test_metrics_saturate_at_uint32_max()
{
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerRtosDrainMetrics metrics{
        maximum, maximum, maximum, maximum, maximum, 0, 0, {}};
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Urgent, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Control, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Normal, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::DroppedInvalid,
                  RuntimeOwnerRtosLane::None, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Fault,
                  RuntimeOwnerRtosLane::Normal, {}});
    return metrics.urgent_processed_count == maximum &&
           metrics.control_processed_count == maximum &&
           metrics.normal_processed_count == maximum &&
           metrics.dropped_invalid_count == maximum &&
           metrics.receive_fault_count == maximum;
}

bool test_activation_preflight_table()
{
    RuntimeOwnerActivationFacts invalid_bool{};
    invalid_bool.queue_drain_ready = 2;
    RuntimeOwnerActivationFacts dirty_reserved{};
    dirty_reserved.reserved[2] = 1;
    const RuntimeOwnerActivationFacts partial{1, 0, 0, 0, 0, {}};
    const RuntimeOwnerActivationFacts ready{1, 1, 1, 1, 1, {}};
    return runtime_owner_activation_preflight(invalid_bool, 9) ==
               RuntimeOwnerActivationPreflightResult::RejectedInvalid &&
           runtime_owner_activation_preflight(dirty_reserved, 9) ==
               RuntimeOwnerActivationPreflightResult::RejectedInvalid &&
           runtime_owner_activation_preflight(ready, 0) ==
               RuntimeOwnerActivationPreflightResult::RejectedInvalid &&
           runtime_owner_activation_preflight(partial, 9) ==
               RuntimeOwnerActivationPreflightResult::RejectedIncomplete &&
           runtime_owner_activation_preflight(ready, 9) ==
               RuntimeOwnerActivationPreflightResult::Ready;
}

bool test_stop_preflight_and_dependency_boundary()
{
    RuntimeOwnerStopFacts invalid{};
    invalid.work_started = 2;
    RuntimeOwnerStopFacts dirty{};
    dirty.reserved[3] = 1;
    const RuntimeOwnerStopFacts safe{0, 0, 1, 1, {}};
    const RuntimeOwnerStopFacts admitted{1, 0, 1, 1, {}};
    const RuntimeOwnerStopFacts started{0, 1, 1, 1, {}};
    const RuntimeOwnerStopFacts queued{0, 0, 0, 1, {}};
    const RuntimeOwnerStopFacts inflight{0, 0, 1, 0, {}};
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT
        "/src/boot_v2/runtime_owner_rtos_drain_core.hpp");
    const bool dependencies_clean =
        !source.empty() && source.find("FreeRTOS.h") == std::string::npos &&
        source.find("queue.h") == std::string::npos &&
        source.find("task.h") == std::string::npos &&
        source.find("new ") == std::string::npos &&
        source.find("malloc") == std::string::npos &&
        source.find("throw") == std::string::npos &&
        source.find("catch") == std::string::npos;
    return runtime_owner_stop_preflight(invalid) ==
               RuntimeOwnerStopPreflightResult::RejectedInvalid &&
           runtime_owner_stop_preflight(dirty) ==
               RuntimeOwnerStopPreflightResult::RejectedInvalid &&
           runtime_owner_stop_preflight(safe) ==
               RuntimeOwnerStopPreflightResult::SafePreAdmissionAbort &&
           runtime_owner_stop_preflight(admitted) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           runtime_owner_stop_preflight(started) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           runtime_owner_stop_preflight(queued) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           runtime_owner_stop_preflight(inflight) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           dependencies_clean;
}

}  // namespace

int main()
{
    CHECK(test_numeric_layout_and_default_contracts());
    CHECK(test_all_empty_reads_no_sink());
    CHECK(test_urgent_precedes_control_and_normal());
    CHECK(test_control_precedes_normal());
    CHECK(test_normal_only());
    CHECK(test_next_pass_rechecks_new_urgent());
    CHECK(test_drain_until_idle_records_once_and_rechecks_urgent());
    CHECK(test_drain_until_idle_records_fault_once_and_stops());
    CHECK(test_drain_until_idle_metrics_count_drop_and_terminal_empty_once());
    CHECK(test_fifo_within_lane());
    CHECK(test_urgent_fault_stops_lower_reads());
    CHECK(test_control_fault_stops_normal_read());
    CHECK(test_normal_fault_calls_no_sink());
    CHECK(test_invalid_urgent_variants_drop());
    CHECK(test_invalid_control_variants_drop());
    CHECK(test_normal_rejection_has_no_fallback());
    CHECK(test_metrics_exact_increment());
    CHECK(test_metrics_saturate_at_uint32_max());
    CHECK(test_activation_preflight_table());
    CHECK(test_stop_preflight_and_dependency_boundary());
    CHECK(test_rtos_source_includes_and_calls_shared_drain());
    CHECK(test_urgent_queue_uses_provenance_message());
    CHECK(test_public_surface_remains_read_only_and_narrow());
    CHECK(test_production_atomic_cutover_is_single_and_ordered());
    CHECK(test_redacted_status_cache_contract_and_publish_point_mutants());
    CHECK(test_balanced_function_scanner_ignores_lexical_braces());

    if (failures != 0) {
        std::printf("FAIL checks=%d failures=%d\n", checks, failures);
        return 1;
    }
    std::printf("PASS checks=%d\n", checks);
    return 0;
}
