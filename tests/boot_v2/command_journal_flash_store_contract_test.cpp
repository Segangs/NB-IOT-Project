#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

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
                code.push_back(' ');
                code.push_back(' ');
                ++index;
                state = SourceLexState::LineComment;
            } else if (current == '/' && next == '*') {
                code.push_back(' ');
                code.push_back(' ');
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
                code.push_back(' ');
                code.push_back(' ');
                ++index;
                state = SourceLexState::Code;
            } else {
                code.push_back(current == '\n' ? '\n' : ' ');
            }
            break;
        case SourceLexState::StringLiteral:
            code.push_back(current == '\n' ? '\n' : ' ');
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                state = SourceLexState::Code;
            }
            break;
        case SourceLexState::CharacterLiteral:
            code.push_back(current == '\n' ? '\n' : ' ');
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '\'') {
                state = SourceLexState::Code;
            }
            break;
        }
    }
    return code;
}

bool is_source_whitespace(const char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

std::string compact_code(const std::string &source)
{
    const std::string code =
        code_without_comments_and_literals(source);
    std::string compact;
    compact.reserve(code.size());
    for (const char value : code) {
        if (!is_source_whitespace(value)) {
            compact.push_back(value);
        }
    }
    return compact;
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
            if (depth == 0) {
                return {};
            }
            --depth;
            if (depth == 0) {
                return source.substr(
                    opening + 1u, index - opening - 1u);
            }
        }
    }
    return {};
}

std::string slot_case_segment(
    const std::string &body,
    const std::string &label)
{
    const std::size_t begin = body.find(label);
    if (begin == std::string::npos) {
        return {};
    }
    std::size_t end = body.find(
        "caseCommandJournalSlot::", begin + label.size());
    if (end == std::string::npos) {
        end = body.find('}', begin + label.size());
    }
    if (end == std::string::npos) {
        return {};
    }
    return body.substr(begin, end - begin);
}

bool slot_case_maps_to(
    const std::string &segment,
    const std::string &expected_offset)
{
    const std::string expected_assignment =
        "offset=" + expected_offset + ";";
    return !segment.empty() &&
           count_occurrences(segment, "offset=") == 1 &&
           count_occurrences(segment, expected_assignment) == 1 &&
           count_occurrences(segment, "returntrue;") == 1;
}

bool adapter_contract_holds(const std::string &source)
{
    const std::string code = compact_code(source);
    const std::string slot_body = braced_body_after(
        code, "boolcommand_journal_flash_slot_offset(");
    const std::string read_body = braced_body_after(
        code, "boolcommand_journal_flash_read_slot(");
    const std::string transaction_anchor =
        "FlashOperationResult"
        "command_journal_flash_replace_transaction(";
    const std::string transaction_body =
        braced_body_after(code, transaction_anchor);
    const std::string replace_body = braced_body_after(
        code, "boolcommand_journal_flash_replace_slot(");
    const std::string port_body = braced_body_after(
        code, "CommandJournalStorePortcommand_journal_flash_port(");
    const std::string load_body = braced_body_after(
        code, "CommandJournalStoreResultcommand_journal_flash_load(");
    const std::string commit_body = braced_body_after(
        code, "CommandJournalStoreResultcommand_journal_flash_commit(");
    if (slot_body.empty() || read_body.empty() ||
        transaction_body.empty() || replace_body.empty() ||
        port_body.empty() || load_body.empty() ||
        commit_body.empty()) {
        return false;
    }

    const std::string slot_a = slot_case_segment(
        slot_body, "caseCommandJournalSlot::A:");
    const std::string slot_b = slot_case_segment(
        slot_body, "caseCommandJournalSlot::B:");
    const std::string slot_none = slot_case_segment(
        slot_body, "caseCommandJournalSlot::None:");
    const bool slot_mapping_holds =
        count_occurrences(
            slot_body, "caseCommandJournalSlot::A:") == 1 &&
        count_occurrences(
            slot_body, "caseCommandJournalSlot::B:") == 1 &&
        count_occurrences(
            slot_body, "caseCommandJournalSlot::None:") == 1 &&
        slot_case_maps_to(
            slot_a,
            "flash_partition::command_journal_a_offset") &&
        slot_case_maps_to(
            slot_b,
            "flash_partition::command_journal_b_offset") &&
        count_occurrences(slot_none, "offset=") == 0 &&
        count_occurrences(slot_none, "returnfalse;") == 1 &&
        slot_body.find("}returnfalse;") != std::string::npos;

    const bool transaction_holds =
        count_occurrences(code, transaction_anchor) == 1 &&
        count_occurrences(
            transaction_body,
            "if(write==nullptr)"
            "{returnFlashOperationCode::InvalidArgument;}") == 1 &&
        count_occurrences(
            transaction_body,
            "constFlashOperationResultresult="
            "transaction.replace_sector("
            "write->offset,write->offset,write->page,"
            "flash_partition::page_size);") == 1 &&
        count_occurrences(
            transaction_body, "transaction.replace_sector(") == 1 &&
        count_occurrences(
            transaction_body, "transaction.read(") == 1 &&
        count_occurrences(
            transaction_body,
            "write->verification_attempted=true;") == 1 &&
        count_occurrences(
            transaction_body, "write->verified=") == 1 &&
        count_occurrences(
            transaction_body,
            "FlashMutationDisposition::NotAttempted") == 1;

    const std::string page_validation =
        "if(page==nullptr||"
        "page_size!=flash_partition::page_size||"
        "reinterpret_cast<std::uintptr_t>(page)%"
        "flash_partition::page_size!=0u){returnfalse;}";
    const std::string service_execute =
        "constFlashOperationResultresult=flash_operation_execute("
        "command_journal_flash_replace_transaction,"
        "&write,timeout_ms);";
    const bool replace_holds =
        count_occurrences(replace_body, page_validation) == 1 &&
        count_occurrences(
            replace_body,
            "if(!command_journal_flash_slot_offset(slot,offset))"
            "{returnfalse;}") == 1 &&
        count_occurrences(
            replace_body,
            "CommandJournalFlashWritewrite{offset,page};") == 1 &&
        count_occurrences(replace_body, service_execute) == 1 &&
        count_occurrences(
            replace_body, "flash_operation_execute(") == 1 &&
        count_occurrences(
            replace_body,
            "FlashMutationDisposition::NotAttempted") == 1 &&
        count_occurrences(
            replace_body,
            "returnwrite.verification_attempted&&"
            "write.verified;") == 1 &&
        count_occurrences(
            replace_body,
            "result==FlashOperationCode::Succeeded") == 0 &&
        count_occurrences(
            replace_body,
            "FlashMutationDisposition::Applied") == 0;

    const bool read_holds =
        count_occurrences(
            read_body,
            "if(!command_journal_flash_slot_offset(slot,offset))"
            "{returnfalse;}") == 1 &&
        count_occurrences(
            read_body,
            "constauto*constsource="
            "reinterpret_cast<conststd::uint8_t*>(XIP_BASE+offset);") ==
            1 &&
        count_occurrences(
            read_body,
            "std::memcpy(&output,source,sizeof(output));") == 1 &&
        count_occurrences(read_body, "std::memcpy(") == 1;

    const bool wrappers_hold =
        count_occurrences(
            port_body,
            "return{nullptr,command_journal_flash_read_slot,"
            "command_journal_flash_replace_slot,};") == 1 &&
        count_occurrences(
            load_body,
            "returncommand_journal_store_load("
            "command_journal_flash_port(),output);") == 1 &&
        count_occurrences(
            commit_body,
            "returncommand_journal_store_commit("
            "command_journal_flash_port(),record,timeout_ms);") == 1;

    const bool central_contract_holds =
        count_occurrences(
            code,
            "static_assert(flash_partition::total_size=="
            "PICO_FLASH_SIZE_BYTES);") == 1 &&
        code.find("command_journal_record_select(") ==
            std::string::npos &&
        code.find("command_journal_record_encode(") ==
            std::string::npos &&
        code.find("command_journal_next_sequence(") ==
            std::string::npos &&
        code.find("flash_safe_execute(") ==
            std::string::npos &&
        code.find("flash_range_erase(") ==
            std::string::npos &&
        code.find("flash_range_program(") ==
            std::string::npos;

    return slot_mapping_holds && read_holds && transaction_holds &&
           replace_holds && wrappers_hold &&
           central_contract_holds;
}

bool dormant_store_reference_absent(
    const std::string &source)
{
    const std::string code = compact_code(source);
    return code.find("command_journal_flash_load") ==
               std::string::npos &&
           code.find("command_journal_flash_commit") ==
               std::string::npos;
}

std::string replace_once(
    std::string source,
    const std::string &from,
    const std::string &to)
{
    const std::size_t position = source.find(from);
    if (position == std::string::npos) {
        return {};
    }
    source.replace(position, from.size(), to);
    return source;
}

std::string swap_once(
    std::string source,
    const std::string &left,
    const std::string &right)
{
    constexpr const char *placeholder =
        "NB_IOT_COMMAND_FLASH_MUTATION_PLACEHOLDER";
    source = replace_once(source, left, placeholder);
    if (source.empty()) {
        return {};
    }
    source = replace_once(source, right, left);
    if (source.empty()) {
        return {};
    }
    return replace_once(source, placeholder, right);
}

void check_adapter_mutant_rejected(
    const char *const name,
    const std::string &mutant,
    const int line) noexcept
{
    CHECK(!mutant.empty());
    const bool rejected = !adapter_contract_holds(mutant);
    if (!rejected) {
        std::fprintf(stderr, "accepted dangerous mutant: %s\n", name);
    }
    check(rejected, name, line);
}

#define CHECK_MUTANT_REJECTED(name, mutant) \
    check_adapter_mutant_rejected((name), (mutant), __LINE__)

std::string root_firmware_source_graph()
{
    const std::string root_cmake = read_source("CMakeLists.txt");
    const std::size_t graph_begin =
        root_cmake.find("add_executable(nb_iot_project");
    if (graph_begin == std::string::npos) {
        return {};
    }
    const std::size_t graph_end =
        root_cmake.find("\n)", graph_begin);
    if (graph_end == std::string::npos) {
        return {};
    }
    return root_cmake.substr(
        graph_begin, graph_end + 2u - graph_begin);
}

std::size_t root_cmake_source_count(const std::string &source_path)
{
    return count_occurrences(
        root_firmware_source_graph(), source_path);
}

std::string trim_source_path(const std::string &line)
{
    const std::size_t begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = line.find_last_not_of(" \t\r");
    return line.substr(begin, end + 1u - begin);
}

void test_adapter_uses_central_rp2350_flash_contract()
{
    const std::string source = read_source(
        "src/boot_v2/command_journal_flash_store.cpp");
    CHECK(adapter_contract_holds(source));
    CHECK(!source.empty());
    CHECK(source.find("flash_safe_execute(") == std::string::npos);
    CHECK(source.find("flash_range_erase(") == std::string::npos);
    CHECK(source.find("flash_range_program(") == std::string::npos);
    CHECK(
        source.find("flash_operation_execute(") !=
        std::string::npos);
    CHECK(
        source.find("transaction.replace_sector(") !=
        std::string::npos);
    CHECK(
        source.find("flash_partition::command_journal_a_offset") !=
        std::string::npos);
    CHECK(
        source.find("flash_partition::command_journal_b_offset") !=
        std::string::npos);
    CHECK(
        source.find("flash_partition::command_journal_slot_size") !=
        std::string::npos);
    CHECK(
        source.find("flash_partition::page_size") !=
        std::string::npos);
    CHECK(source.find("PICO_FLASH_SIZE_BYTES") != std::string::npos);
    CHECK(source.find("XIP_BASE") != std::string::npos);
    CHECK(source.find("std::memcpy(") != std::string::npos);
    CHECK(
        source.find("command_journal_store_load(") !=
        std::string::npos);
    CHECK(
        source.find("command_journal_store_commit(") !=
        std::string::npos);
    CHECK(
        source.find("command_journal_record_select(") ==
        std::string::npos);
    CHECK(
        source.find("command_journal_record_encode(") ==
        std::string::npos);
    CHECK(
        source.find("command_journal_next_sequence(") ==
        std::string::npos);
}

void test_adapter_structure_mutants_are_rejected()
{
    const std::string source = read_source(
        "src/boot_v2/command_journal_flash_store.cpp");
    CHECK(adapter_contract_holds(source));

    const std::string slot_a_assignment =
        "offset = flash_partition::command_journal_a_offset;";
    const std::string slot_b_assignment =
        "offset = flash_partition::command_journal_b_offset;";
    CHECK_MUTANT_REJECTED(
        "reversed A/B mapping",
        swap_once(source, slot_a_assignment, slot_b_assignment));
    CHECK_MUTANT_REJECTED(
        "same A/B mapping",
        replace_once(
            source,
            slot_b_assignment,
            "offset = flash_partition::command_journal_a_offset;\n"
            "        // flash_partition::command_journal_b_offset"));
    CHECK_MUTANT_REJECTED(
        "one-byte XIP read",
        replace_once(
            source,
            "std::memcpy(&output, source, sizeof(output));",
            "std::memcpy(&output, source, 1u);"));

    const std::string replace_call =
        "transaction.replace_sector(\n"
        "        write->offset,\n"
        "        write->offset,\n"
        "        write->page,\n"
        "        flash_partition::page_size);";
    CHECK_MUTANT_REJECTED(
        "wrong replace-sector erase offset",
        replace_once(
            source,
            replace_call,
            "transaction.replace_sector(\n"
            "        flash_partition::command_journal_a_offset,\n"
            "        write->offset,\n"
            "        write->page,\n"
            "        flash_partition::page_size);"));
    CHECK_MUTANT_REJECTED(
        "wrong replace-sector page offset",
        replace_once(
            source,
            replace_call,
            "transaction.replace_sector(\n"
            "        write->offset,\n"
            "        flash_partition::command_journal_b_offset,\n"
            "        write->page,\n"
            "        flash_partition::page_size);"));
    CHECK_MUTANT_REJECTED(
        "wrong replace-sector page size",
        replace_once(
            source,
            replace_call,
            "transaction.replace_sector(\n"
            "        write->offset,\n"
            "        write->offset,\n"
            "        write->page,\n"
            "        flash_partition::command_journal_slot_size);"));
    CHECK_MUTANT_REJECTED(
        "missing transaction result propagation",
        replace_once(
            source,
            replace_call,
            "FlashOperationCode::Succeeded;"));
    CHECK_MUTANT_REJECTED(
        "missing null transaction context rejection",
        replace_once(
            source,
            "if (write == nullptr) {\n"
            "        return FlashOperationCode::InvalidArgument;\n"
            "    }\n",
            ""));
    CHECK_MUTANT_REJECTED(
        "bypassed serialized verification",
        replace_once(
            source,
            "return write.verification_attempted &&\n"
            "           write.verified;",
            "return true;"));
    CHECK_MUTANT_REJECTED(
        "missing serialized readback",
        replace_once(
            source,
            "const FlashOperationResult read_result = transaction.read(",
            "const FlashOperationResult read_result = "
            "FlashOperationCode::Succeeded; // transaction.read("));
    CHECK_MUTANT_REJECTED(
        "missing exact page-size rejection",
        replace_once(
            source,
            "page_size != flash_partition::page_size ||",
            "false ||"));
    CHECK_MUTANT_REJECTED(
        "missing page-alignment rejection",
        replace_once(
            source,
            "reinterpret_cast<std::uintptr_t>(page) %\n"
            "                flash_partition::page_size !=\n"
            "            0u",
            "false"));
}

void test_dormant_reference_mutants_are_rejected()
{
    CHECK(dormant_store_reference_absent(
        "runtime_owner_poll();"));
    CHECK(!dormant_store_reference_absent(
        "command_journal_flash_load(record);"));
    CHECK(!dormant_store_reference_absent(
        "command_journal_flash_load (record);"));
    CHECK(!dormant_store_reference_absent(
        "auto load = &command_journal_flash_load;"));
    CHECK(!dormant_store_reference_absent(
        "auto commit = command_journal_flash_commit;"));
}

void test_public_wrapper_contract_is_declared()
{
    const std::string header = read_source(
        "src/boot_v2/command_journal_flash_store.hpp");
    CHECK(!header.empty());
    CHECK(
        header.find("command_journal_flash_load(") !=
        std::string::npos);
    CHECK(
        header.find("command_journal_flash_commit(") !=
        std::string::npos);
    CHECK(
        header.find("CommandJournalStoreResult") !=
        std::string::npos);
    CHECK(
        header.find("std::uint32_t timeout_ms") !=
        std::string::npos);
}

void test_root_firmware_source_graph_registers_adapter_once()
{
    CHECK(!root_firmware_source_graph().empty());
    CHECK(
        root_cmake_source_count(
            "src/boot_v2/command_journal_flash_store.cpp") == 1);
    CHECK(
        root_cmake_source_count(
            "src/boot_v2/command_journal_store_core.cpp") == 1);
}

void test_runtime_owner_and_tasks_do_not_call_dormant_store()
{
    const std::string graph = root_firmware_source_graph();
    std::istringstream lines(graph);
    std::string line;
    std::size_t guarded_sources = 0;
    while (std::getline(lines, line)) {
        const bool is_runtime_owner =
            line.find("src/boot_v2/runtime_owner") !=
            std::string::npos;
        const bool is_task =
            line.find("src/tasks/") != std::string::npos;
        const bool is_main =
            line.find("main.cpp") != std::string::npos;
        if (!is_runtime_owner && !is_task && !is_main) {
            continue;
        }

        const std::string path = trim_source_path(line);
        const std::string source = read_source(path);
        ++guarded_sources;
        CHECK(!source.empty());
        CHECK(dormant_store_reference_absent(source));
    }
    CHECK(guarded_sources != 0);
}

} // namespace

int main()
{
    test_adapter_uses_central_rp2350_flash_contract();
    test_adapter_structure_mutants_are_rejected();
    test_dormant_reference_mutants_are_rejected();
    test_public_wrapper_contract_is_declared();
    test_root_firmware_source_graph_registers_adapter_once();
    test_runtime_owner_and_tasks_do_not_call_dormant_store();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "command_journal_flash_store_contract_test: "
            "%zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "command_journal_flash_store_contract_test: "
        "%zu checks passed\n",
        g_checks);
    return 0;
}
