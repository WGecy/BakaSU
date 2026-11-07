// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#ifndef YUKILINKER_HPP
#define YUKILINKER_HPP

#include <type_traits>
#include <utility>

#include "yukilinker.h"

#if __cplusplus < 201703L
#error YukiLinker requires C++17
#endif
#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif

namespace yukilinker {

enum class Backend : uint32_t {
  Private = YUKILINKER_C_BACKEND_PRIVATE,
  System = YUKILINKER_C_BACKEND_SYSTEM
};
enum class Execution : uint32_t {
  NotStarted = YUKILINKER_C_EXECUTION_NOT_STARTED,
  Started = YUKILINKER_C_EXECUTION_STARTED,
  Unknown = YUKILINKER_C_EXECUTION_UNKNOWN
};
enum class Reference : uint32_t {
  None = YUKILINKER_C_REFERENCE_NONE,
  Owned = YUKILINKER_C_REFERENCE_OWNED,
  Released = YUKILINKER_C_REFERENCE_RELEASED,
  Unknown = YUKILINKER_C_REFERENCE_UNKNOWN
};
enum class LeaseKind : uint32_t {
  Observer = YUKILINKER_C_LEASE_OBSERVER,
  Execution = YUKILINKER_C_LEASE_EXECUTION,
  Unwind = YUKILINKER_C_LEASE_UNWIND,
  Cfi = YUKILINKER_C_LEASE_CFI,
  HostBridge = YUKILINKER_C_LEASE_HOST_BRIDGE
};
enum class Code : uint32_t {
  Ok = YUKILINKER_C_OK,
  InvalidElf = YUKILINKER_C_INVALID_ELF,
  Unsupported = YUKILINKER_C_UNSUPPORTED,
  DependencyMissing = YUKILINKER_C_DEPENDENCY_MISSING,
  SymbolMissing = YUKILINKER_C_SYMBOL_MISSING,
  PermissionDenied = YUKILINKER_C_PERMISSION_DENIED,
  NoMemory = YUKILINKER_C_NO_MEMORY,
  IoError = YUKILINKER_C_IO_ERROR,
  Busy = YUKILINKER_C_BUSY,
  InvalidHandle = YUKILINKER_C_INVALID_HANDLE,
  Retry = YUKILINKER_C_RETRY,
  BufferTooSmall = YUKILINKER_C_BUFFER_TOO_SMALL,
  StaleToken = YUKILINKER_C_STALE_TOKEN
};

struct Error {
  yukilinker_c_error native{};
  Code code() const noexcept { return static_cast<Code>(native.detail.code); }
  Execution execution() const noexcept { return static_cast<Execution>(native.execution); }
  bool cleanup_incomplete() const noexcept { return native.detail.cleanup_incomplete != 0; }
  explicit operator bool() const noexcept { return code() != Code::Ok; }
};

template <class T>
struct Result {
  T value{};
  Error error{};
  explicit operator bool() const noexcept { return !error; }
};

struct CloseResult {
  yukilinker_c_close_result native{};
  Code code() const noexcept { return static_cast<Code>(native.error.detail.code); }
  bool handle_valid() const noexcept { return native.handle_valid != 0; }
  Reference reference() const noexcept { return static_cast<Reference>(native.info.reference); }
  uint32_t retention_reasons() const noexcept { return native.info.retention_reasons; }
  bool private_mapping_unmapped() const noexcept {
    return native.info.private_mapping_unmapped != 0;
  }
};

struct ShutdownResult {
  bool complete = false;
  yukilinker_c_error_detail error{};
};

struct OpenOptions {
  yukilinker_c_open_options native{};
  explicit OpenOptions(Backend backend = Backend::Private) noexcept {
    native.size = sizeof(native);
    native.backend = static_cast<uint32_t>(backend);
    native.flags = RTLD_NOW;
  }
};

// All objects below own existing C resources, never an additional registry. The
// owner and Core must outlive every child. Destruction diagnoses omitted explicit
// release; it never runs module destructors or silently discards retained ownership.
// adopt() transfers an existing C ownership token, not a borrowed alias.
class Module {
 public:
  Module() noexcept = default;
  static Module adopt(yukilinker_c_runtime* owner, yukilinker_c_handle handle) noexcept {
    Module result;
    result.owner_ = owner;
    result.handle_ = handle;
    return result;
  }
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  Module& operator=(Module&&) = delete;
  Module(Module&& other) noexcept { swap(other); }
  ~Module() {
    if (*this) yukilinker_c_report_unreleased(owner_, "yukilinker: Module needs explicit close");
  }
  void swap(Module& other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(handle_, other.handle_);
    std::swap(last_, other.last_);
  }
  explicit operator bool() const noexcept { return handle_.slot != 0 && handle_.generation != 0; }
  yukilinker_c_handle native_handle() const noexcept { return handle_; }
  const CloseResult& last_close() const noexcept { return last_; }
  CloseResult close() noexcept {
    last_.native = yukilinker_c_close(owner_, handle_);
    if (!last_.handle_valid()) handle_ = {};
    return last_;
  }
  Code forget() noexcept {
    const auto code = yukilinker_c_forget(owner_, handle_);
    if (code == YUKILINKER_C_OK) handle_ = {};
    return static_cast<Code>(code);
  }
  Code release_host() noexcept {
    const auto code = yukilinker_c_release_host(owner_, handle_);
    if (code == YUKILINKER_C_OK) handle_ = {};
    return static_cast<Code>(code);
  }
  Result<yukilinker_c_handle_info> query() const noexcept {
    Result<yukilinker_c_handle_info> result;
    result.error.native.detail.code = yukilinker_c_query(owner_, handle_, &result.value);
    return result;
  }
  Result<yukilinker_c_image_record> image() const noexcept {
    Result<yukilinker_c_image_record> result;
    result.error.native.detail.code = yukilinker_c_handle_image(owner_, handle_, &result.value);
    return result;
  }
  // T is a function/object pointer with the exact exported ABI. The caller owns
  // that declaration and must hold the module/lease while using the returned address.
  template <class T>
  Result<T> symbol(const char* name, const char* version = nullptr) const noexcept {
    static_assert(std::is_pointer_v<T>, "symbol<T> requires a pointer type");
    const auto found = yukilinker_c_lookup(owner_, handle_, name, version);
    return {reinterpret_cast<T>(found.address), Error{found.error}};
  }

 private:
  yukilinker_c_runtime* owner_ = nullptr;
  yukilinker_c_handle handle_{};
  CloseResult last_{};
};

class Context {
 public:
  Context() noexcept = default;
  static Context adopt(yukilinker_c_runtime* owner, yukilinker_c_context* handle) noexcept {
    Context result;
    result.owner_ = owner;
    result.handle_ = handle;
    return result;
  }
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context& operator=(Context&&) = delete;
  Context(Context&& other) noexcept { swap(other); }
  ~Context() {
    if (*this) yukilinker_c_report_unreleased(owner_, "yukilinker: Context needs explicit destroy");
  }
  void swap(Context& other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(handle_, other.handle_);
  }
  explicit operator bool() const noexcept { return handle_ != nullptr; }
  yukilinker_c_context* native_handle() const noexcept { return handle_; }
  bool destroy() noexcept {
    if (!yukilinker_c_context_destroy(owner_, handle_)) return false;
    handle_ = nullptr;
    return true;
  }

 private:
  yukilinker_c_runtime* owner_ = nullptr;
  yukilinker_c_context* handle_ = nullptr;
};

class Namespace {
 public:
  Namespace() noexcept = default;
  static Namespace adopt(yukilinker_c_runtime* owner, yukilinker_c_namespace* handle) noexcept {
    Namespace result;
    result.owner_ = owner;
    result.handle_ = handle;
    return result;
  }
  Namespace(const Namespace&) = delete;
  Namespace& operator=(const Namespace&) = delete;
  Namespace& operator=(Namespace&&) = delete;
  Namespace(Namespace&& other) noexcept { swap(other); }
  ~Namespace() {
    if (*this)
      yukilinker_c_report_unreleased(owner_, "yukilinker: Namespace needs explicit destroy");
  }
  void swap(Namespace& other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(handle_, other.handle_);
  }
  explicit operator bool() const noexcept { return handle_ != nullptr; }
  yukilinker_c_namespace* native_handle() const noexcept { return handle_; }
  uint64_t id() const noexcept { return yukilinker_c_namespace_id(owner_, handle_); }
  Code destroy() noexcept {
    const auto code = yukilinker_c_namespace_destroy(owner_, handle_);
    if (code == YUKILINKER_C_OK) handle_ = nullptr;
    return static_cast<Code>(code);
  }
  Code link(const Namespace* target, uint32_t flags, const char* sonames = nullptr) noexcept {
    if (target && owner_ != target->owner_) return Code::StaleToken;
    return static_cast<Code>(yukilinker_c_namespace_link(
        owner_, handle_, target ? target->handle_ : nullptr, flags, sonames));
  }
  Code import(const yukilinker_c_host_import& request) noexcept {
    return static_cast<Code>(yukilinker_c_namespace_host_import(owner_, handle_, &request));
  }

 private:
  yukilinker_c_runtime* owner_ = nullptr;
  yukilinker_c_namespace* handle_ = nullptr;
};

class Snapshot {
 public:
  Snapshot() noexcept = default;
  static Snapshot adopt(yukilinker_c_runtime* owner, yukilinker_c_snapshot token,
                        size_t count) noexcept {
    Snapshot result;
    result.owner_ = owner;
    result.token_ = token;
    result.count_ = count;
    return result;
  }
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot& operator=(Snapshot&&) = delete;
  Snapshot(Snapshot&& other) noexcept { swap(other); }
  ~Snapshot() {
    if (*this)
      yukilinker_c_report_unreleased(owner_, "yukilinker: Snapshot needs explicit release");
  }
  void swap(Snapshot& other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(token_, other.token_);
    std::swap(count_, other.count_);
  }
  explicit operator bool() const noexcept { return token_.value != 0; }
  yukilinker_c_snapshot native_handle() const noexcept { return token_; }
  size_t size() const noexcept { return count_; }
  Code release() noexcept {
    const auto code = yukilinker_c_snapshot_release(owner_, &token_);
    if (!token_.value) count_ = 0;
    return static_cast<Code>(code);
  }
  Result<yukilinker_c_image_record> image(size_t index) const noexcept {
    Result<yukilinker_c_image_record> result;
    result.error.native.detail.code =
        yukilinker_c_snapshot_image(owner_, token_, index, &result.value);
    return result;
  }
  Result<yukilinker_c_mapping_record> mapping(size_t index) const noexcept {
    Result<yukilinker_c_mapping_record> result;
    result.error.native.detail.code =
        yukilinker_c_snapshot_mapping(owner_, token_, index, &result.value);
    return result;
  }

 private:
  yukilinker_c_runtime* owner_ = nullptr;
  yukilinker_c_snapshot token_{};
  size_t count_ = 0;
};

class Lease {
 public:
  Lease() noexcept = default;
  static Lease adopt(yukilinker_c_runtime* owner, yukilinker_c_lease token) noexcept {
    Lease result;
    result.owner_ = owner;
    result.token_ = token;
    return result;
  }
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  Lease& operator=(Lease&&) = delete;
  Lease(Lease&& other) noexcept { swap(other); }
  ~Lease() {
    if (*this) yukilinker_c_report_unreleased(owner_, "yukilinker: Lease needs explicit release");
  }
  void swap(Lease& other) noexcept {
    std::swap(owner_, other.owner_);
    std::swap(token_, other.token_);
  }
  explicit operator bool() const noexcept { return token_.value != 0; }
  yukilinker_c_lease native_handle() const noexcept { return token_; }
  Code release() noexcept { return static_cast<Code>(yukilinker_c_image_release(owner_, &token_)); }

 private:
  yukilinker_c_runtime* owner_ = nullptr;
  yukilinker_c_lease token_{};
};

class Runtime {
 public:
  Runtime() noexcept = default;
  static Runtime adopt(yukilinker_c_runtime* handle) noexcept {
    Runtime result;
    result.handle_ = handle;
    return result;
  }
  static Runtime create(const yukilinker_c_host_services& services) noexcept {
    return adopt(yukilinker_c_runtime_create(&services));
  }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime& operator=(Runtime&&) = delete;
  Runtime(Runtime&& other) noexcept { swap(other); }
  ~Runtime() {
    if (*this)
      yukilinker_c_report_unreleased(handle_, "yukilinker: Runtime needs explicit destroy");
  }
  void swap(Runtime& other) noexcept { std::swap(handle_, other.handle_); }
  explicit operator bool() const noexcept { return handle_ != nullptr; }
  yukilinker_c_runtime* native_handle() const noexcept { return handle_; }
  ShutdownResult try_shutdown() noexcept {
    ShutdownResult result;
    result.complete = yukilinker_c_try_shutdown(handle_, &result.error) != 0;
    return result;
  }
  bool destroy() noexcept {
    if (!yukilinker_c_runtime_destroy(handle_)) return false;
    handle_ = nullptr;
    return true;
  }
  Code bind(const yukilinker_c_host_services* services) noexcept {
    return static_cast<Code>(yukilinker_c_host_services_bind(handle_, services));
  }
  Code limits(uint32_t handles, uint64_t stub_bytes) noexcept {
    return static_cast<Code>(yukilinker_c_set_limits(handle_, handles, stub_bytes));
  }
  Result<yukilinker_c_runtime_stats> stats() const noexcept {
    Result<yukilinker_c_runtime_stats> result;
    result.error.native.detail.code = yukilinker_c_query_stats(handle_, &result.value);
    return result;
  }
  Context context(const char* path = nullptr, yukilinker_c_dependency_provider provider = nullptr,
                  void* user = nullptr) noexcept {
    return Context::adopt(handle_, yukilinker_c_context_create(handle_, path, provider, user));
  }
  Namespace namespace_create(const Context* parent = nullptr) noexcept {
    return Namespace::adopt(handle_, yukilinker_c_namespace_create(
                                         handle_, parent ? parent->native_handle() : nullptr));
  }
  Result<Module> open_path(const char* path, const OpenOptions& options = OpenOptions{}) noexcept {
    yukilinker_c_error error{};
    const auto token = yukilinker_c_open_path(handle_, path, &options.native, &error);
    return {Module::adopt(handle_, token), Error{error}};
  }
  Result<Module> open_fd(int fd, const OpenOptions& options = OpenOptions{}) noexcept {
    yukilinker_c_error error{};
    const auto token = yukilinker_c_open_fd(handle_, fd, &options.native, &error);
    return {Module::adopt(handle_, token), Error{error}};
  }
  Result<Snapshot> snapshot(const Context* context = nullptr,
                            const Namespace* scope = nullptr) noexcept {
    yukilinker_c_snapshot token{};
    size_t count = 0;
    Error error;
    error.native.detail.code =
        yukilinker_c_snapshot_acquire(handle_, context ? context->native_handle() : nullptr,
                                      scope ? scope->native_handle() : nullptr, &token, &count);
    return {Snapshot::adopt(handle_, token, count), error};
  }
  Result<Lease> lease(yukilinker_c_image_id image, LeaseKind kind = LeaseKind::Observer) noexcept {
    yukilinker_c_lease token{};
    Error error;
    error.native.detail.code =
        yukilinker_c_image_acquire(handle_, image, static_cast<uint32_t>(kind), &token);
    return {Lease::adopt(handle_, token), error};
  }

 private:
  yukilinker_c_runtime* handle_ = nullptr;
};

}  // namespace yukilinker
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
#endif /* YUKILINKER_HPP */
