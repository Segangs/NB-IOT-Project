#ifndef NB_IOT_FLASH_OPERATION_SERVICE_HPP
#define NB_IOT_FLASH_OPERATION_SERVICE_HPP

#include <cstddef>
#include <cstdint>

namespace boot_v2 {

enum class FlashOperationCode : std::uint8_t {
    Succeeded = 0,
    InvalidArgument = 1,
    InvalidRange = 2,
    InvalidAlignment = 3,
    LockUnavailable = 4,
    TimedOut = 5,
    PlatformFailure = 6,
};

enum class FlashMutationDisposition : std::uint8_t {
    NotAttempted = 0,
    Applied = 1,
    Unknown = 2,
};

struct FlashOperationResult final {
    FlashOperationCode code{FlashOperationCode::PlatformFailure};
    FlashMutationDisposition mutation{
        FlashMutationDisposition::NotAttempted};
    bool deadline_exceeded{false};

    constexpr FlashOperationResult() noexcept = default;

    constexpr FlashOperationResult(
        const FlashOperationCode operation_code,
        const FlashMutationDisposition mutation_disposition =
            FlashMutationDisposition::NotAttempted,
        const bool operation_deadline_exceeded = false) noexcept
        : code(operation_code),
          mutation(mutation_disposition),
          deadline_exceeded(operation_deadline_exceeded)
    {
    }

    [[nodiscard]] constexpr bool succeeded() const noexcept
    {
        return code == FlashOperationCode::Succeeded;
    }
};

[[nodiscard]] constexpr bool operator==(
    const FlashOperationResult result,
    const FlashOperationCode code) noexcept
{
    return result.code == code;
}

[[nodiscard]] constexpr bool operator==(
    const FlashOperationCode code,
    const FlashOperationResult result) noexcept
{
    return result == code;
}

[[nodiscard]] constexpr bool operator!=(
    const FlashOperationResult result,
    const FlashOperationCode code) noexcept
{
    return !(result == code);
}

[[nodiscard]] constexpr bool operator!=(
    const FlashOperationCode code,
    const FlashOperationResult result) noexcept
{
    return !(result == code);
}

class FlashOperationTransaction;

using FlashOperationCallback = FlashOperationResult (*)(
    FlashOperationTransaction &,
    void *) noexcept;

class FlashOperationTransaction final {
public:
    FlashOperationTransaction(
        const FlashOperationTransaction &) = delete;
    FlashOperationTransaction &operator=(
        const FlashOperationTransaction &) = delete;
    FlashOperationTransaction(
        FlashOperationTransaction &&) = delete;
    FlashOperationTransaction &operator=(
        FlashOperationTransaction &&) = delete;

    [[nodiscard]] FlashOperationResult read(
        std::uint32_t offset,
        void *output,
        std::size_t size) noexcept;

    [[nodiscard]] FlashOperationResult erase_range(
        std::uint32_t offset,
        std::size_t size) noexcept;

    [[nodiscard]] FlashOperationResult program_page(
        std::uint32_t offset,
        const std::uint8_t *page,
        std::size_t size) noexcept;

    [[nodiscard]] FlashOperationResult replace_sector(
        std::uint32_t sector_offset,
        std::uint32_t page_offset,
        const std::uint8_t *page,
        std::size_t page_size) noexcept;

private:
    explicit FlashOperationTransaction(
        std::uint64_t deadline_us,
        bool pre_scheduler) noexcept;

    [[nodiscard]] FlashOperationResult execute_raw_operation(
        std::uint32_t erase_offset,
        std::size_t erase_size,
        std::uint32_t program_offset,
        const std::uint8_t *program_data,
        std::size_t program_size) noexcept;

    std::uint64_t deadline_us_{0};
    bool pre_scheduler_{false};
    bool reconciliation_read_available_{false};

    friend FlashOperationResult flash_operation_execute(
        FlashOperationCallback,
        void *,
        std::uint32_t) noexcept;
};

[[nodiscard]] FlashOperationResult flash_operation_execute(
    FlashOperationCallback callback,
    void *context,
    std::uint32_t timeout_ms) noexcept;

} // namespace boot_v2

#endif
