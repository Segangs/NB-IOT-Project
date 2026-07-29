#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(
    const bool condition,
    const char *const expression,
    const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(
            stderr,
            "CHECK failed: %s:%d: %s\n",
            __FILE__,
            line,
            expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

std::string read_source(const std::string &relative_path)
{
    const std::string path =
        std::string(NB_IOT_SOURCE_ROOT) + "/" + relative_path;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

enum class SourceLexState {
    Code,
    LineComment,
    BlockComment,
    StringLiteral,
    CharacterLiteral,
};

std::string code_without_comments_and_literals(
    const std::string &source)
{
    std::string code;
    code.reserve(source.size());
    SourceLexState state = SourceLexState::Code;
    bool escaped = false;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char current = source[index];
        const char next =
            index + 1u < source.size() ? source[index + 1u] : '\0';
        switch (state) {
        case SourceLexState::Code:
            if (current == '/' && next == '/') {
                code.append(2u, ' ');
                ++index;
                state = SourceLexState::LineComment;
            } else if (current == '/' && next == '*') {
                code.append(2u, ' ');
                ++index;
                state = SourceLexState::BlockComment;
            } else if (current == '"') {
                code.push_back(' ');
                state = SourceLexState::StringLiteral;
                escaped = false;
            } else if (current == '\'') {
                code.push_back(' ');
                state = SourceLexState::CharacterLiteral;
                escaped = false;
            } else {
                code.push_back(current);
            }
            break;
        case SourceLexState::LineComment:
            if (current == '\n') {
                code.push_back('\n');
                state = SourceLexState::Code;
            } else {
                code.push_back(' ');
            }
            break;
        case SourceLexState::BlockComment:
            if (current == '*' && next == '/') {
                code.append(2u, ' ');
                ++index;
                state = SourceLexState::Code;
            } else {
                code.push_back(current == '\n' ? '\n' : ' ');
            }
            break;
        case SourceLexState::StringLiteral:
        case SourceLexState::CharacterLiteral: {
            code.push_back(current == '\n' ? '\n' : ' ');
            const char terminal =
                state == SourceLexState::StringLiteral ? '"' : '\'';
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == terminal) {
                state = SourceLexState::Code;
            }
            break;
        }
        }
    }
    return code;
}

std::string compact_code(const std::string &source)
{
    const std::string code =
        code_without_comments_and_literals(source);
    std::string compact;
    compact.reserve(code.size());
    for (const char value : code) {
        if (value != ' ' && value != '\t' && value != '\n' &&
            value != '\r' && value != '\f' && value != '\v') {
            compact.push_back(value);
        }
    }
    return compact;
}

std::size_t count_occurrences(
    const std::string &source,
    const std::string &needle) noexcept
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = source.find(needle, position)) !=
           std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::vector<std::string> identifier_tokens(
    const std::string &source)
{
    const std::string code =
        code_without_comments_and_literals(source);
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < code.size()) {
        const unsigned char current =
            static_cast<unsigned char>(code[index]);
        const bool identifier_start =
            (current >= 'A' && current <= 'Z') ||
            (current >= 'a' && current <= 'z') ||
            current == '_';
        if (!identifier_start) {
            ++index;
            continue;
        }
        const std::size_t start = index++;
        while (index < code.size()) {
            const unsigned char value =
                static_cast<unsigned char>(code[index]);
            const bool identifier_part =
                (value >= 'A' && value <= 'Z') ||
                (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') ||
                value == '_';
            if (!identifier_part) {
                break;
            }
            ++index;
        }
        tokens.push_back(code.substr(start, index - start));
    }
    return tokens;
}

std::string braced_body_after(
    const std::string &source,
    const std::string &anchor)
{
    const std::size_t anchor_position = source.find(anchor);
    if (anchor_position == std::string::npos) {
        return {};
    }
    const std::size_t opening =
        source.find('{', anchor_position + anchor.size());
    if (opening == std::string::npos) {
        return {};
    }

    std::size_t depth = 0;
    for (std::size_t index = opening; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            if (depth == 0u) {
                return {};
            }
            --depth;
            if (depth == 0u) {
                return source.substr(
                    opening + 1u, index - opening - 1u);
            }
        }
    }
    return {};
}

bool production_source_extension(
    const std::filesystem::path &path) noexcept
{
    const std::string extension = path.extension().string();
    return extension == ".cpp" || extension == ".hpp" ||
           extension == ".c" || extension == ".h";
}

void test_raw_flash_mutation_has_exactly_one_owner()
{
    const std::string expected_owner =
        "src/lib/flash_operation_service.cpp";
    const std::vector<std::string> raw_primitives{
        "flash_safe_execute",
        "flash_range_erase",
        "flash_range_program",
    };
    const std::vector<std::string> multicore_launchers{
        "multicore_launch_core1",
        "multicore_launch_core1_with_stack",
        "multicore_launch_core1_raw",
    };
    const std::filesystem::path source_root =
        std::filesystem::path(NB_IOT_SOURCE_ROOT);
    std::vector<std::filesystem::path> production_files{
        source_root / "main.cpp",
    };
    for (const char *const directory : {"lib", "src"}) {
        const std::filesystem::path root =
            source_root / directory;
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() &&
                production_source_extension(entry.path())) {
                production_files.push_back(entry.path());
            }
        }
    }

    std::size_t owner_files = 0;
    std::unordered_map<std::string, std::size_t> token_counts;
    for (const std::filesystem::path &path : production_files) {
        const std::string relative =
            std::filesystem::relative(
                path, NB_IOT_SOURCE_ROOT)
                .generic_string();
        const std::vector<std::string> tokens =
            identifier_tokens(read_source(relative));
        bool owns_raw_primitive = false;
        for (const std::string &token : tokens) {
            for (const std::string &primitive : raw_primitives) {
                if (token == primitive) {
                    ++token_counts[primitive];
                    owns_raw_primitive = true;
                }
            }
            for (const std::string &launcher : multicore_launchers) {
                if (token == launcher) {
                    ++token_counts[launcher];
                }
            }
        }
        if (!owns_raw_primitive) {
            continue;
        }
        ++owner_files;
        CHECK(relative == expected_owner);
    }

    CHECK(owner_files == 1u);
    CHECK(token_counts["flash_safe_execute"] == 1u);
    CHECK(token_counts["flash_range_erase"] == 1u);
    CHECK(token_counts["flash_range_program"] == 1u);
    for (const std::string &launcher : multicore_launchers) {
        CHECK(token_counts[launcher] == 0u);
    }
}

void test_service_surface_is_synchronous_and_bounded()
{
    const std::string header =
        read_source("src/lib/flash_operation_service.hpp");
    const std::string source =
        read_source("src/lib/flash_operation_service.cpp");
    const std::string root_cmake = read_source("CMakeLists.txt");

    CHECK(!header.empty());
    CHECK(!source.empty());
    CHECK(
        header.find("enum class FlashOperationCode") !=
        std::string::npos);
    CHECK(
        header.find("enum class FlashMutationDisposition") !=
        std::string::npos);
    CHECK(
        header.find("struct FlashOperationResult") !=
        std::string::npos);
    CHECK(
        header.find("class FlashOperationTransaction") !=
        std::string::npos);
    CHECK(header.find("FlashOperationCallback") != std::string::npos);
    CHECK(header.find("flash_operation_execute(") != std::string::npos);
    CHECK(header.find("read(") != std::string::npos);
    CHECK(header.find("erase_range(") != std::string::npos);
    CHECK(header.find("program_page(") != std::string::npos);
    CHECK(header.find("replace_sector(") != std::string::npos);
    const std::string header_code = compact_code(header);
    CHECK(
        header_code.find(
            "FlashOperationTransaction("
            "constFlashOperationTransaction&)=delete;") !=
        std::string::npos);
    CHECK(
        header_code.find(
            "FlashOperationTransaction&operator=("
            "constFlashOperationTransaction&)=delete;") !=
        std::string::npos);
    CHECK(
        header_code.find(
            "FlashOperationTransaction("
            "FlashOperationTransaction&&)=delete;") !=
        std::string::npos);
    CHECK(
        header_code.find(
            "FlashOperationTransaction&operator=("
            "FlashOperationTransaction&&)=delete;") !=
        std::string::npos);
    CHECK(
        count_occurrences(
            root_cmake,
            "src/lib/flash_operation_service.cpp") == 1u);

    const std::string code = compact_code(source);
    CHECK(code.find("StaticSemaphore_t") != std::string::npos);
    CHECK(
        count_occurrences(code, "xSemaphoreCreateMutexStatic(") ==
        1u);
    CHECK(code.find("xSemaphoreCreateMutex(") == std::string::npos);
    CHECK(code.find("pvPortMalloc(") == std::string::npos);
    CHECK(code.find("xSemaphoreTake(") != std::string::npos);
    CHECK(code.find("xSemaphoreGive(") != std::string::npos);
    CHECK(code.find("xTaskGetSchedulerState(") != std::string::npos);
    CHECK(code.find("xPortIsInsideInterrupt(") != std::string::npos);
    CHECK(code.find("taskSCHEDULER_NOT_STARTED") != std::string::npos);
    CHECK(code.find("taskSCHEDULER_RUNNING") != std::string::npos);
    CHECK(code.find("taskSCHEDULER_SUSPENDED") != std::string::npos);
    CHECK(code.find("UINT32_MAX") != std::string::npos);
    CHECK(code.find("portMAX_DELAY") == std::string::npos);
}

void test_pre_scheduler_gate_and_single_deadline_are_bounded()
{
    const std::string header = compact_code(
        read_source("src/lib/flash_operation_service.hpp"));
    const std::string source = compact_code(
        read_source("src/lib/flash_operation_service.cpp"));
    const std::string execute_body = braced_body_after(
        source,
        "FlashOperationResultflash_operation_execute(");
    const std::string raw_body = braced_body_after(
        source,
        "FlashOperationTransaction::execute_raw_operation(");

    CHECK(header.find("TimedOut") != std::string::npos);
    CHECK(header.find("NotAttempted") != std::string::npos);
    CHECK(header.find("Applied") != std::string::npos);
    CHECK(header.find("Unknown") != std::string::npos);
    CHECK(header.find("deadline_exceeded") != std::string::npos);
    CHECK(
        header.find("std::uint64_tdeadline_us_") !=
        std::string::npos);
    CHECK(
        source.find("std::atomic_flagg_pre_scheduler_gate") !=
        std::string::npos);
    CHECK(
        source.find(
            "test_and_set(std::memory_order_acquire)") !=
        std::string::npos);
    CHECK(
        source.find("clear(std::memory_order_release)") !=
        std::string::npos);
    CHECK(
        source.find("acquire_pre_scheduler_gate(") !=
        std::string::npos);
    CHECK(
        source.find("time_us_64()") != std::string::npos);
    CHECK(
        source.find(
            "MAXIMUM_FLASH_OPERATION_TIMEOUT_MS=60000u") !=
        std::string::npos);
    CHECK(
        source.find("std::numeric_limits<TickType_t>::max()") !=
        std::string::npos);
    CHECK(
        source.find(
            "static_cast<std::uint64_t>(timeout_ms)*"
            "configTICK_RATE_HZ") !=
        std::string::npos);

    const std::size_t deadline_creation =
        execute_body.find("deadline_us=");
    const std::size_t remaining_before_lock =
        execute_body.find("remaining_timeout_ms(deadline_us)");
    const std::size_t mutex_take =
        execute_body.find("xSemaphoreTake(");
    CHECK(deadline_creation != std::string::npos);
    CHECK(remaining_before_lock != std::string::npos);
    CHECK(mutex_take != std::string::npos);
    CHECK(deadline_creation < remaining_before_lock);
    CHECK(remaining_before_lock < mutex_take);
    CHECK(
        execute_body.find(
            "acquire_pre_scheduler_gate(deadline_us)") !=
        std::string::npos);

    const std::size_t raw_remaining =
        raw_body.find("remaining_timeout_ms(deadline_us_)");
    const std::size_t safe_execute =
        raw_body.find("flash_safe_execute(");
    CHECK(raw_remaining != std::string::npos);
    CHECK(safe_execute != std::string::npos);
    CHECK(raw_remaining < safe_execute);
    CHECK(raw_body.find("remaining_ms/2u") != std::string::npos);
    CHECK(
        raw_body.find("callback_entered") != std::string::npos);
    CHECK(
        raw_body.find("callback_completed") != std::string::npos);
    CHECK(
        raw_body.find("save_and_disable_interrupts(") !=
        std::string::npos);
    CHECK(
        raw_body.find("restore_interrupts(") != std::string::npos);
}

void test_raw_platform_result_mapping_is_exact()
{
    const std::string source = compact_code(
        read_source("src/lib/flash_operation_service.cpp"));
    CHECK(
        source.find(
            "FlashMutationDisposition::NotAttempted") !=
        std::string::npos);
    CHECK(
        source.find("FlashMutationDisposition::Applied") !=
        std::string::npos);
    CHECK(
        source.find("FlashMutationDisposition::Unknown") !=
        std::string::npos);
    CHECK(
        source.find(
            "operation.callback_entered&&"
            "!operation.callback_completed") !=
        std::string::npos);
}

void test_service_validates_before_raw_mutation()
{
    const std::string source = compact_code(
        read_source("src/lib/flash_operation_service.cpp"));
    const std::string raw_callback = braced_body_after(
        source,
        "void__no_inline_not_in_flash_func("
        "flash_operation_raw_callback)(void*parameter)");
    const std::string replace_body = braced_body_after(
        source,
        "FlashOperationResultFlashOperationTransaction::"
        "replace_sector(");
    const std::string erase_body = braced_body_after(
        source,
        "FlashOperationResultFlashOperationTransaction::erase_range(");
    const std::string program_body = braced_body_after(
        source,
        "FlashOperationResultFlashOperationTransaction::program_page(");

    CHECK(
        source.find("range_within_writable_partition(") !=
        std::string::npos);
    CHECK(source.find("size==0u") != std::string::npos);
    CHECK(
        source.find("size>flash_partition::total_size-offset") !=
        std::string::npos);
    CHECK(
        source.find("offset%flash_partition::sector_size") !=
        std::string::npos);
    CHECK(
        source.find("offset%flash_partition::page_size") !=
        std::string::npos);
    CHECK(!raw_callback.empty());
    CHECK(!replace_body.empty());
    CHECK(!erase_body.empty());
    CHECK(!program_body.empty());
    CHECK(
        raw_callback.find("flash_range_erase(") <
        raw_callback.find("flash_range_program("));
    CHECK(
        raw_callback.find("taskENTER_CRITICAL") ==
        std::string::npos);
    CHECK(
        raw_callback.find("taskEXIT_CRITICAL") ==
        std::string::npos);
    CHECK(
        replace_body.find("valid_erase_range(") <
        replace_body.find("execute_raw_operation("));
    CHECK(
        replace_body.find("valid_program_page(") <
        replace_body.find("execute_raw_operation("));
}

void test_clients_keep_complete_transactions_and_failure_order()
{
    const std::string logger = compact_code(
        read_source("src/lib/flash_logger.cpp"));
    const std::string journal = compact_code(
        read_source(
            "src/boot_v2/command_journal_flash_store.cpp"));
    const std::string shutdown = compact_code(
        read_source(
            "src/boot_v2/runtime_owner_shutdown_record_store.cpp"));

    for (const std::string *const client :
         {&logger, &journal, &shutdown}) {
        CHECK(client->find("flash_safe_execute(") ==
              std::string::npos);
        CHECK(client->find("flash_range_erase(") ==
              std::string::npos);
        CHECK(client->find("flash_range_program(") ==
              std::string::npos);
        CHECK(client->find("flash_operation_execute(") !=
              std::string::npos);
    }

    const std::string logger_transaction = braced_body_after(
        logger,
        "FlashOperationResultflash_log_write_transaction(");
    const std::string logger_scan = braced_body_after(
        logger,
        "FlashOperationResultscan_flash_log(");
    const std::size_t page_read =
        logger_transaction.find("transaction.read(");
    const std::size_t erase_decision =
        logger_transaction.find("constboolneed_erase=");
    const std::size_t erased_page_reset =
        logger_transaction.find(
            "if(need_erase){"
            "memset(page_buffer,0xFF,sizeof(page_buffer));}");
    const std::size_t entry_overlay =
        logger_transaction.find(
            "memcpy(page_buffer+entry_offset_in_page,"
            "&context->entry,sizeof(FlashLogEntry));");
    const std::size_t page_mutation =
        logger_transaction.find("transaction.replace_sector(");
    const std::size_t offset_commit =
        logger_transaction.find("g_write_offset=");
    const std::size_t initialized_commit =
        logger_transaction.find("g_initialized=true;");
    CHECK(!logger_scan.empty());
    CHECK(
        logger_scan.find("g_write_offset=") ==
        std::string::npos);
    CHECK(
        logger_scan.find("g_initialized=") ==
        std::string::npos);
    CHECK(!logger_transaction.empty());
    CHECK(
        logger_transaction.find("scan_flash_log(") !=
        std::string::npos);
    CHECK(
        logger_transaction.find("initialize_flash_log(") ==
        std::string::npos);
    CHECK(page_read != std::string::npos);
    CHECK(erase_decision != std::string::npos);
    CHECK(erased_page_reset != std::string::npos);
    CHECK(entry_overlay != std::string::npos);
    CHECK(page_mutation != std::string::npos);
    CHECK(initialized_commit != std::string::npos);
    CHECK(offset_commit != std::string::npos);
    CHECK(page_read < erase_decision);
    CHECK(erase_decision < erased_page_reset);
    CHECK(erased_page_reset < entry_overlay);
    CHECK(entry_overlay < page_mutation);
    CHECK(page_mutation < initialized_commit);
    CHECK(initialized_commit < offset_commit);
    const std::size_t unknown_reconcile =
        logger_transaction.find(
            "result.mutation=="
            "boot_v2::FlashMutationDisposition::Unknown");
    const std::size_t applied_gate =
        logger_transaction.find(
            "result.mutation!="
            "boot_v2::FlashMutationDisposition::Applied");
    CHECK(unknown_reconcile != std::string::npos);
    CHECK(applied_gate != std::string::npos);
    CHECK(unknown_reconcile < applied_gate);
    CHECK(applied_gate < initialized_commit);
    CHECK(
        logger_transaction.find("g_initialized=false;") !=
        std::string::npos);

    const std::string journal_transaction = braced_body_after(
        journal,
        "FlashOperationResultcommand_journal_flash_replace_transaction(");
    const std::string journal_replace = braced_body_after(
        journal,
        "boolcommand_journal_flash_replace_slot(");
    CHECK(
        journal_transaction.find(
            "constFlashOperationResultresult="
            "transaction.replace_sector(") !=
        std::string::npos);
    CHECK(
        journal_transaction.find("transaction.read(") !=
        std::string::npos);
    CHECK(
        journal_transaction.find(
            "write->verification_attempted=true;") !=
        std::string::npos);
    CHECK(
        journal.find(
            "flash_operation_execute("
            "command_journal_flash_replace_transaction,") !=
        std::string::npos);
    CHECK(
        journal_replace.find(
            "result==FlashOperationCode::Succeeded") ==
        std::string::npos);
    CHECK(
        journal.find(
            "result.mutation=="
            "FlashMutationDisposition::NotAttempted") !=
        std::string::npos);
    CHECK(
        journal.find(
            "returnwrite.verification_attempted&&"
            "write.verified;") !=
        std::string::npos);
    CHECK(
        journal_transaction.find(
            "std::memcmp(readback,write->page,sizeof(readback))") !=
        std::string::npos);

    const std::string shutdown_transaction = braced_body_after(
        shutdown,
        "FlashOperationResultruntime_owner_shutdown_record_transaction(");
    CHECK(
        shutdown_transaction.find(
            "constFlashOperationResultresult="
            "transaction.replace_sector(") !=
        std::string::npos);
    CHECK(
        shutdown_transaction.find("transaction.read(") !=
        std::string::npos);
    CHECK(
        shutdown_transaction.find(
            "write->verification_attempted=true;") !=
        std::string::npos);
    CHECK(
        shutdown.find(
            "constFlashOperationResultresult=flash_operation_execute("
            "runtime_owner_shutdown_record_transaction,") !=
        std::string::npos);
    CHECK(
        shutdown.find("result!=FlashOperationCode::Succeeded") ==
        std::string::npos);
    CHECK(
        shutdown.find(
            "FlashMutationDisposition::NotAttempted") !=
        std::string::npos);
    CHECK(
        shutdown.find(
            "returnwrite.verification_attempted&&"
            "write.verified;") !=
        std::string::npos);
    CHECK(
        shutdown.find(
            "constRuntimeOwnerShutdownRecordV1&written=") ==
        std::string::npos);
}

} // namespace

int main()
{
    test_raw_flash_mutation_has_exactly_one_owner();
    test_service_surface_is_synchronous_and_bounded();
    test_pre_scheduler_gate_and_single_deadline_are_bounded();
    test_raw_platform_result_mapping_is_exact();
    test_service_validates_before_raw_mutation();
    test_clients_keep_complete_transactions_and_failure_order();

    if (g_failures != 0u) {
        std::fprintf(
            stderr,
            "flash_operation_service_contract_test: "
            "%zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "flash_operation_service_contract_test: "
        "%zu checks passed\n",
        g_checks);
    return 0;
}
