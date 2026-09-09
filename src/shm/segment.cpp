#include <fcntl.h>    // O_CREAT, O_RDWR, ...
#include <sys/mman.h> // shm_open, shm_unlink, mmap, munmap, PROT_*, MAP_*
#include <sys/stat.h> // permission bits: S_IRUSR, S_IWUSR, ...
#include <unistd.h>   // ftruncate, close

#include "shm/segment.hpp"

#include <cerrno>
#include <utility>

namespace lle::shm {

namespace {

void close_descriptor(int descriptor) {
    static_cast<void>(close(descriptor));
}

void clean_up_failed_creation(const std::string& name, int descriptor) noexcept {
    close_descriptor(descriptor);
    static_cast<void>(shm_unlink(name.c_str()));
}

} // namespace

std::expected<SharedMemorySegment, SegmentError>
SharedMemorySegment::create_shm(std::string name, std::size_t size) noexcept {
    if (!is_shm_name_valid(name)) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Validate, .error_number = EINVAL}
        );
    }

    constexpr mode_t permissions = S_IRUSR | S_IWUSR;
    const int descriptor =
        shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, permissions);
    if (descriptor == -1) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Open, .error_number = errno}
        );
    }

    if (ftruncate(descriptor, static_cast<off_t>(size)) == -1) {
        const int error_number = errno;
        clean_up_failed_creation(name, descriptor);
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Resize, .error_number = error_number}
        );
    }

    void* const mapping =
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
        const int error_number = errno;
        clean_up_failed_creation(name, descriptor);
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Map, .error_number = error_number}
        );
    }

    return SharedMemorySegment{
        std::move(name),
        descriptor,
        static_cast<std::byte*>(mapping),
        size,
        true
    };
}

SharedMemorySegment::SharedMemorySegment(
    std::string name,
    int descriptor,
    std::byte* address,
    std::size_t size,
    bool owner
) noexcept
    : name_(std::move(name)),
      descriptor_(descriptor),
      address_(address),
      size_(size),
      owner_(owner),
      linked_(true) {}

SharedMemorySegment::~SharedMemorySegment() {
    static_cast<void>(close());
}

SharedMemorySegment::SharedMemorySegment(SharedMemorySegment&& other) noexcept
    : name_(std::move(other.name_)),
      descriptor_(std::exchange(other.descriptor_, -1)),
      address_(std::exchange(other.address_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      owner_(std::exchange(other.owner_, false)),
      linked_(std::exchange(other.linked_, false)) {}

SharedMemorySegment& SharedMemorySegment::operator=(SharedMemorySegment&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    static_cast<void>(close());

    name_ = std::move(other.name_);
    descriptor_ = std::exchange(other.descriptor_, -1);
    address_ = std::exchange(other.address_, nullptr);
    size_ = std::exchange(other.size_, 0);
    owner_ = std::exchange(other.owner_, false);
    linked_ = std::exchange(other.linked_, false);
    return *this;
}

std::expected<void, SegmentError> SharedMemorySegment::close() noexcept {
    int unmap_error = 0;

    if (address_ != nullptr) {
        if (munmap(address_, size_) == -1) {
            unmap_error = errno;
        } else {
            address_ = nullptr;
            size_ = 0;
        }
    }

    int close_error = 0;
    if (descriptor_ != -1) {
        if (::close(descriptor_) == -1) {
            close_error = errno;
        }
        descriptor_ = -1;
    }

    if (unmap_error != 0) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Unmap, .error_number = unmap_error}
        );
    }
    if (close_error != 0) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Close, .error_number = close_error}
        );
    };
    return {};
}

std::expected<void, SegmentError> SharedMemorySegment::unlink() noexcept {
    if (!owner_) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Unlink, .error_number = EPERM}
        );
    }

    if (!linked_) {
        return {};
    }

    if (shm_unlink(name_.c_str()) == -1) {
        if (errno == ENOENT) {
            linked_ = false;
            return {};
        }
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Unlink, .error_number = errno}
        );
    }

    linked_ = false;
    return {};
}

std::expected<SharedMemorySegment, SegmentError>
SharedMemorySegment::attach_shm(std::string name) noexcept {
    if (!is_shm_name_valid(name)) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Validate, .error_number = errno}
        );
    }

    const int descriptor = shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    if (descriptor == -1) {
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Open, .error_number = errno}
        );
    }

    struct stat status{};
    if (fstat(descriptor, &status) == -1) {
        const int error_number = errno;
        close_descriptor(descriptor);
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Stat, .error_number = error_number}
        );
    }

    const __off_t sz = status.st_size;
    if (sz <= 0) {
        close_descriptor(descriptor);
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Attach, .error_number = EINVAL}
        );
    }

    const auto size = static_cast<std::size_t>(sz);

    void* const mapping =
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);

    if (mapping == MAP_FAILED) {
        const int error_number = errno;
        clean_up_failed_creation(name, descriptor);
        return std::unexpected(
            SegmentError{.operation = SegmentOperation::Map, .error_number = error_number}
        );
    }

    return SharedMemorySegment(
        std::move(name),
        descriptor,
        static_cast<std::byte*>(mapping),
        size,
        false
    );
}

bool SharedMemorySegment::is_shm_name_valid(std::string_view name) noexcept {
    if (name.size() < 2 || name.size() > max_segment_name_length) {
        return false;
    }

    if (name.front() != '/') {
        return false;
    }

    constexpr std::string_view allowed_characters = "abcdefghijklmnopqrstuvwxyz"
                                                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                    "0123456789._-";

    const std::string_view identifier = name.substr(1);
    return identifier != "." && identifier != ".." &&
           identifier.find_first_not_of(allowed_characters) == std::string_view::npos;
}

} // namespace lle::shm
