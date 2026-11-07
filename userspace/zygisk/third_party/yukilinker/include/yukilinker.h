// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#ifndef YUKILINKER_H
#define YUKILINKER_H

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YUKILINKER_VERSION_MAJOR 1
#define YUKILINKER_VERSION_MINOR 0
#define YUKILINKER_VERSION_PATCH 0
#define YUKILINKER_VERSION_STRING "1.0.0"
#define YUKILINKER_C_ABI_VERSION 1U

/* Runtime code belongs to the containing Core. Even a Core with default symbol
 * visibility must bind these functions to its own static/source copy. */
#if defined(__GNUC__)
#define YUKILINKER_C_API __attribute__((visibility("hidden")))
#else
#define YUKILINKER_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yukilinker_c_runtime yukilinker_c_runtime;
typedef struct yukilinker_c_context yukilinker_c_context;
typedef struct yukilinker_c_namespace yukilinker_c_namespace;
typedef struct yukilinker_c_snapshot {
  uint64_t value;
} yukilinker_c_snapshot;
typedef struct yukilinker_c_lease {
  uint64_t value;
} yukilinker_c_lease;

enum yukilinker_c_error_code {
  YUKILINKER_C_OK,
  YUKILINKER_C_INVALID_ELF,
  YUKILINKER_C_UNSUPPORTED,
  YUKILINKER_C_DEPENDENCY_MISSING,
  YUKILINKER_C_SYMBOL_MISSING,
  YUKILINKER_C_PERMISSION_DENIED,
  YUKILINKER_C_NO_MEMORY,
  YUKILINKER_C_IO_ERROR,
  YUKILINKER_C_BUSY,
  YUKILINKER_C_INVALID_HANDLE,
  YUKILINKER_C_RETRY,
  YUKILINKER_C_BUFFER_TOO_SMALL,
  YUKILINKER_C_STALE_TOKEN
};

enum yukilinker_c_stage {
  YUKILINKER_C_SOURCE,
  YUKILINKER_C_METADATA,
  YUKILINKER_C_DEPENDENCIES,
  YUKILINKER_C_RELOCATIONS,
  YUKILINKER_C_PROTECTION,
  YUKILINKER_C_INITIALIZATION,
  YUKILINKER_C_RETIREMENT
};

// This interface is shared with C hosts.
// NOLINTBEGIN(modernize-use-using, modernize-redundant-void-arg)
/* A failed operation. system_errno is the errno of the system call that failed, set only with
 * PERMISSION_DENIED (EACCES, EPERM), NO_MEMORY (ENOMEM) or IO_ERROR (any other errno); a failure no
 * system call reported carries 0. NO_MEMORY names a limit of the address space or of the mappings
 * a process may have, not necessarily exhausted physical memory. offset depends on the stage:
 * with RELOCATIONS it is the relocation's r_offset, a virtual address relative to the image's
 * load bias; with PROTECTION and INVALID_ELF it is the file offset of the program header that was
 * refused; otherwise it is 0. */
typedef struct yukilinker_c_error_detail {
  uint32_t code;
  uint32_t stage;
  int32_t system_errno;
  uint32_t relocation_type;
  uint64_t offset;
  uint32_t machine;
  uint32_t execution_started;
  uint32_t cleanup_incomplete;
  char object[256];
  char symbol[128];
  char version[128];
} yukilinker_c_error_detail;

// A successful provider transfers ownership of its descriptor to the runtime.
typedef int (*yukilinker_c_dependency_provider)(void* user, const char* requester,
                                                const char* name);

typedef struct yukilinker_c_image_info {
  uintptr_t load_bias;
  uintptr_t mapping_start;
  size_t mapping_size;
  const char* name;
  uint64_t generation;
} yukilinker_c_image_info;

typedef int (*yukilinker_c_image_visitor)(const yukilinker_c_image_info*, void*);

enum yukilinker_c_image_phase {
  YUKILINKER_C_IMAGE_MAPPING,
  YUKILINKER_C_IMAGE_RELOCATED,
  YUKILINKER_C_IMAGE_INITIALIZING,
  YUKILINKER_C_IMAGE_READY,
  YUKILINKER_C_IMAGE_CLOSING,
  YUKILINKER_C_IMAGE_RETIRED,
  YUKILINKER_C_IMAGE_FAILED
};

enum yukilinker_c_runtime_event_type {
  YUKILINKER_C_EVENT_PUBLISH,
  YUKILINKER_C_EVENT_RELOCATE,
  YUKILINKER_C_EVENT_CONSTRUCTOR,
  YUKILINKER_C_EVENT_READY,
  YUKILINKER_C_EVENT_CLOSE,
  YUKILINKER_C_EVENT_DESTRUCTOR,
  YUKILINKER_C_EVENT_RETIRE,
  YUKILINKER_C_EVENT_CLEANUP_FAILURE,
  YUKILINKER_C_EVENT_LOAD_FAILURE
};

enum yukilinker_c_nodelete_reason {
  YUKILINKER_C_NODELETE_REQUEST = 1U << 0,
  YUKILINKER_C_NODELETE_ELF = 1U << 1,
  /* Set on every image of a failed open's group once any code of that group ran (a constructor
   * or an IFUNC resolver, also when the loading thread ended inside the open). Terminal: those
   * images stay Failed and mapped until the process ends, and every retirement that reaches one
   * (context_destroy, namespace_destroy, try_shutdown) keeps answering YUKILINKER_C_BUSY, which
   * here never clears. A failure before any code ran sets no reason and retires at the next
   * collection. Each reason is its own bit: RTLD_NODELETE from the open (REQUEST),
   * DF_1_NODELETE (ELF), a fork child (FORK) and the others may join it. */
  YUKILINKER_C_NODELETE_FAILED_EXECUTION = 1U << 2,
  /* The image was kept although its last reference went, because something was running for it
   * when a collection reached it. It is not RTLD_NODELETE (flags stay as they were). Held only
   * by a runtime frame that ran its code (a constructor, a destructor, a resolver, a callback),
   * it retires at the first collection after that frame returned. Held by a call its own code
   * made into the runtime (an interposed dlopen, dlsym, dlclose or __cxa_finalize, found by the
   * return address or a caller stub), it stays until its context or namespace is explicitly
   * retired (context_destroy, namespace_destroy, try_shutdown for the default context): that code
   * runs on after the call returns, which the runtime cannot see end. The caller of that retirement
   * guarantees no code of the scope still runs. A call the image's code makes inside a runtime
   * frame running that same image's code is part of the frame and holds nothing of its own. */
  YUKILINKER_C_NODELETE_CALLBACK_REENTRY = 1U << 3,
  YUKILINKER_C_NODELETE_FORK = 1U << 4,
  YUKILINKER_C_NODELETE_SELF_CLOSE = 1U << 5
};

enum yukilinker_c_lease_kind {
  YUKILINKER_C_LEASE_OBSERVER,
  YUKILINKER_C_LEASE_EXECUTION,
  YUKILINKER_C_LEASE_UNWIND,
  YUKILINKER_C_LEASE_CFI,
  YUKILINKER_C_LEASE_HOST_BRIDGE
};

typedef struct yukilinker_c_image_id {
  uint64_t generation;
  uint64_t namespace_id;
} yukilinker_c_image_id;

/* Frozen: every call that copies an image record writes the whole struct and sets size to
 * sizeof(yukilinker_c_image_record) as output; it never reads the caller's size. The layout never
 * grows: more fields come through a new entry point (image_detail). */
typedef struct yukilinker_c_image_record {
  uint32_t size;
  uint32_t phase;
  yukilinker_c_image_id id;
  uintptr_t load_bias;
  uintptr_t mapping_start;
  size_t mapping_size;
  uint64_t device;
  uint64_t inode;
  uint64_t source_offset;
  uint64_t flags;
  uint64_t program_header_count;
  /* The runtime's own TLS module id (0: none). A private image's dl_iterate_phdr entry reports
   * dlpi_tls_modid 0 instead: this id is no index the system's __tls_get_addr knows. */
  uint64_t tls_module_id;
  uint64_t tls_generation;
  uint64_t open_references;
  uint64_t tls_references;
  uint64_t destructor_references;
  uint64_t callback_references;
  uint64_t snapshot_references;
  uint64_t lease_references;
  uint64_t execution_leases;
  uint64_t unwind_leases;
  uint64_t cfi_leases;
  uint64_t host_bridge_leases;
  uint64_t dependency_references;
  uint64_t private_dependencies;
  uint64_t system_dependencies;
  uint32_t nodelete_reasons;
  uint32_t failure_code;
  char origin[4096];
} yukilinker_c_image_record;

enum yukilinker_c_mapping_flag {
  YUKILINKER_C_MAPPING_FILE_BACKED = 1U << 0,
  YUKILINKER_C_MAPPING_CALLER_RESERVED = 1U << 1,
  YUKILINKER_C_MAPPING_WRITE_RELRO = 1U << 2,
  YUKILINKER_C_MAPPING_USE_RELRO = 1U << 3
};

/* Frozen as yukilinker_c_image_record is: written whole, size is output only, the layout never
 * grows. */
typedef struct yukilinker_c_mapping_record {
  uint32_t size;
  uint32_t flags;
  uint64_t relro_written_bytes;
  uint64_t relro_shared_bytes;
} yukilinker_c_mapping_record;

/* A zero id names no image. A YUKILINKER_C_EVENT_CLEANUP_FAILURE with a zero id and an empty origin
 * reports something the runtime keeps for shutdown to release; with a zero id and an origin, a
 * library the system linker failed to close (it stays loaded, nothing retries it): a host import,
 * the origin its library name, or a system dependency of a private image, the origin that image's
 * (the first 127 bytes); with a nonzero id, that image stays registered. */
typedef struct yukilinker_c_runtime_event {
  uint64_t sequence;
  yukilinker_c_image_id id;
  uint32_t type;
  uint32_t phase;
  uint32_t error_code;
  uint32_t reserved;
  char origin[128];
} yukilinker_c_runtime_event;

enum yukilinker_c_image_detail_flag {
  /* failure_stage holds the stage the failed load reported. */
  YUKILINKER_C_DETAIL_FAILURE_STAGE = 1U << 0,
  /* The image's namespace was created from a parent. That parent's id is 0 when it is the
   * default context or an explicit context, so the flag tells it apart from having no parent. */
  YUKILINKER_C_DETAIL_NAMESPACE_PARENT = 1U << 1,
  /* protect_relro has run (relocation pass 2), so every RELRO range, if any, is read-only. */
  YUKILINKER_C_DETAIL_RELRO_PROTECTED = 1U << 2
};

/* A copied view of one image. The caller sets size to the bytes it provides; the runtime writes
 * the largest known version that fits and stores the number of bytes written in size. Addresses
 * are 64-bit on both ABIs. The 128-byte layout below is version 1 and is frozen; a later version
 * only appends fields. Such a version is a new struct of its own (this one stays 128 bytes, so a
 * caller built against this header always provides 128 and gets version 1), and a caller of it
 * that provides more bytes reads size to see how many were written. A size below 128 is refused,
 * and the runtime never writes past size. */
typedef struct yukilinker_c_image_detail {
  uint32_t size;
  uint32_t flags;
  yukilinker_c_image_id id;
  uint32_t failure_code;
  uint32_t failure_stage;
  /* Only for YUKILINKER_C_IO_ERROR, YUKILINKER_C_NO_MEMORY and YUKILINKER_C_PERMISSION_DENIED; 0
   * otherwise. Best effort: a failure that no system call caused can carry a stale value. */
  int32_t failure_errno;
  /* yukilinker_c_mapping_flag bits, as in yukilinker_c_mapping_record. */
  uint32_t mapping_flags;
  uint64_t namespace_parent_id;
  uint64_t tls_initial_image;
  uint64_t tls_file_bytes;
  uint64_t tls_memory_bytes;
  uint64_t tls_alignment;
  /* The calling thread's block for this image; 0 until the thread first touches the TLS. */
  uint64_t tls_block;
  /* PT_GNU_RELRO headers with a non-zero p_memsz. */
  uint64_t relro_segment_count;
  /* The page-rounded range from the lowest to the highest of those segments; 0 if none is
   * valid. */
  uint64_t relro_start;
  uint64_t relro_size;
  uint64_t relro_written_bytes;
  uint64_t relro_shared_bytes;
} yukilinker_c_image_detail;

enum yukilinker_c_dependency_kind {
  YUKILINKER_C_DEPENDENCY_PRIVATE,
  YUKILINKER_C_DEPENDENCY_SYSTEM,
  /* Not resolved yet, or its private target has retired. */
  YUKILINKER_C_DEPENDENCY_UNRESOLVED
};

enum yukilinker_c_dependency_flag {
  /* Added for a symbol the image binds to, not for a DT_NEEDED entry; such an edge has no name. */
  YUKILINKER_C_DEPENDENCY_PROVIDER = 1U << 0,
  /* name holds only the first 255 bytes of a longer DT_NEEDED string. */
  YUKILINKER_C_DEPENDENCY_NAME_TRUNCATED = 1U << 1,
  /* Resolved through one of the namespace's links (namespace_link), outside the namespace. */
  YUKILINKER_C_DEPENDENCY_LINKED = 1U << 2
};

typedef struct yukilinker_c_dependency_record {
  uint32_t kind;
  uint32_t flags;
  /* The private target; zero for other kinds. */
  yukilinker_c_image_id target;
  char name[256];
} yukilinker_c_dependency_record;

enum yukilinker_c_namespace_link_flag {
  /* The link leads to the system linker; to is NULL. */
  YUKILINKER_C_NAMESPACE_LINK_SYSTEM = 1U << 0,
  /* The link shares every name; shared_sonames is NULL or empty. */
  YUKILINKER_C_NAMESPACE_LINK_ALL = 1U << 1
};

typedef struct yukilinker_c_namespace_link_record {
  uint32_t flags;
  uint32_t reserved;
  /* The target namespace's id; 0 exactly when flags has YUKILINKER_C_NAMESPACE_LINK_SYSTEM. */
  uint64_t target_namespace_id;
  /* The ':'-separated sonames the link shares, as given; empty with
   * YUKILINKER_C_NAMESPACE_LINK_ALL. */
  char shared_sonames[4096];
} yukilinker_c_namespace_link_record;

/* One library of the system linker whose listed symbols a namespace's images may bind to. The
 * caller sets size to sizeof(yukilinker_c_host_import); flags must be 0. Borrowed for the call
 * only: both strings are copied before namespace_host_import returns. */
typedef struct yukilinker_c_host_import {
  uint32_t size;
  uint32_t flags;
  /* A library the system linker has already loaded, as dlopen names it: 1-255 bytes. */
  const char* library;
  /* The ':'-separated symbol names it supplies, each 1-255 bytes without ':' or '@', 4095 bytes
   * in all. */
  const char* symbols;
} yukilinker_c_host_import;

/* A copied host import, as namespace_host_import registered it. */
typedef struct yukilinker_c_host_import_record {
  uint32_t flags;
  uint32_t reserved;
  char library[256];
  char symbols[4096];
} yukilinker_c_host_import_record;

/* What a thread in a load_waits snapshot is doing. */
enum yukilinker_c_load_wait_kind {
  /* It is loading a root open: thread and owner are the same; image is that open's root, zero
   * until the root exists (an open that is still finding or waiting for its file has none). */
  YUKILINKER_C_LOAD_WAIT_LOAD = 1,
  /* It waits for owner's open of image, or for owner's collection (image zero). */
  YUKILINKER_C_LOAD_WAIT_WAIT = 2
};

/* One thread of a load_waits snapshot. The caller sets size to sizeof(yukilinker_c_load_wait) in
 * the first record; every record written has it. thread and owner are kernel thread ids. elapsed_ns
 * is how long the load or the wait has lasted, on CLOCK_MONOTONIC. */
typedef struct yukilinker_c_load_wait {
  uint32_t size;
  uint32_t kind;
  int32_t thread;
  int32_t owner;
  yukilinker_c_image_id image;
  uint64_t elapsed_ns;
} yukilinker_c_load_wait;

/* Why a host import refused to bind a symbol (host_import_refusal). Values are stable. */
enum yukilinker_c_host_import_reason {
  /* The library's own entry: neither GLOBAL nor WEAK, or hidden or internal. */
  YUKILINKER_C_HOST_IMPORT_NOT_GLOBAL_OR_WEAK = 1,
  YUKILINKER_C_HOST_IMPORT_HIDDEN = 2,
  /* Its type: TLS, an IFUNC, an absolute value, a reserved section index, or a type the
   * relocation (or a private dlsym) cannot take. */
  YUKILINKER_C_HOST_IMPORT_TLS = 3,
  YUKILINKER_C_HOST_IMPORT_IFUNC = 4,
  YUKILINKER_C_HOST_IMPORT_ABSOLUTE = 5,
  YUKILINKER_C_HOST_IMPORT_RESERVED_SECTION = 6,
  YUKILINKER_C_HOST_IMPORT_TYPE_MISMATCH = 7,
  /* The system's answer: dlsym found nothing, an address outside every library of that name, in
   * the mappings of two of them, or not the value the owner's own entry gives. */
  YUKILINKER_C_HOST_IMPORT_NOT_FOUND_BY_SYSTEM = 8,
  YUKILINKER_C_HOST_IMPORT_OUTSIDE_MAPPINGS = 9,
  YUKILINKER_C_HOST_IMPORT_TWO_OWNERS = 10,
  YUKILINKER_C_HOST_IMPORT_NOT_SYSTEM_VALUE = 11,
  /* Where it lies: a function outside the library's executable, file-backed bytes, data outside
   * its readable ones. */
  YUKILINKER_C_HOST_IMPORT_NOT_CODE = 12,
  YUKILINKER_C_HOST_IMPORT_NOT_DATA = 13
};

/* The refusal that decided the calling thread's last failed operation (host_import_refusal). The
 * caller sets size to sizeof(yukilinker_c_host_import_refusal). Every string is copied and
 * NUL-terminated; version is empty for an unversioned symbol. */
typedef struct yukilinker_c_host_import_refusal {
  uint32_t size;
  uint32_t reason;
  /* The import's library, as namespace_host_import named it. */
  char library[256];
  char symbol[128];
  char version[128];
} yukilinker_c_host_import_refusal;

// NOLINTEND(modernize-use-using, modernize-redundant-void-arg)

struct dl_phdr_info;

// NOLINTBEGIN(modernize-use-using)
typedef struct yukilinker_c_host_dlopen_request {
  uint32_t size;
  int32_t flags;
  const char* name;
  // An android_dlextinfo pointer, or null.
  const void* extension;
  // The private image that caused the request; zero when there is none.
  yukilinker_c_image_id requester;
} yukilinker_c_host_dlopen_request;

enum yukilinker_c_host_fork_phase {
  YUKILINKER_C_HOST_FORK_PREPARE,
  YUKILINKER_C_HOST_FORK_PARENT,
  YUKILINKER_C_HOST_FORK_CHILD
};

typedef void (*yukilinker_c_host_log)(void* user, uint32_t level, const char* message,
                                      size_t length);
typedef void* (*yukilinker_c_host_dlopen)(void* user,
                                          const yukilinker_c_host_dlopen_request* request);
// Returns 1 and stores the address, which may be null, when the symbol exists.
typedef int (*yukilinker_c_host_dlsym)(void* user, void* handle, const char* name,
                                       const char* version, void** address);
typedef int (*yukilinker_c_host_dlclose)(void* user, void* handle);
typedef int (*yukilinker_c_host_dladdr)(void* user, const void* address, Dl_info* info);
typedef char* (*yukilinker_c_host_dlerror)(void* user);
typedef int (*yukilinker_c_host_iterate_phdr)(void* user,
                                              int (*callback)(struct dl_phdr_info*, size_t, void*),
                                              void* data);
typedef size_t (*yukilinker_c_host_page_size)(void* user);
typedef int (*yukilinker_c_host_close_fd)(void* user, int fd);
typedef void (*yukilinker_c_host_fork_hook)(void* user, uint32_t phase);

typedef struct yukilinker_c_host_services {
  uint32_t size;
  uint32_t version;
  void* user;
  // Required. Every system linker call made by the runtime goes through these.
  yukilinker_c_host_dlopen system_dlopen;
  yukilinker_c_host_dlsym system_dlsym;
  yukilinker_c_host_dlclose system_dlclose;
  yukilinker_c_host_dladdr system_dladdr;
  yukilinker_c_host_dlerror system_dlerror;
  yukilinker_c_host_iterate_phdr system_iterate_phdr;
  // Optional. A null entry selects the runtime default.
  yukilinker_c_host_log log;
  // A power of two from the kernel's page (AT_PAGESZ) to 16 KiB; anything else refuses the
  // runtime_create or host_services_bind that asked for it.
  yukilinker_c_host_page_size page_size;
  yukilinker_c_host_close_fd close_fd;
  yukilinker_c_host_fork_hook fork_hook;
} yukilinker_c_host_services;
// NOLINTEND(modernize-use-using)

#define YUKILINKER_C_HOST_SERVICES_VERSION 1U

/* Both words are opaque. Carry them together with the owner; do not interpret
 * slot as an array index. A handle from a different owner/Core is rejected. */
typedef struct yukilinker_c_handle {
  uint64_t slot;
  uint64_t generation;
} yukilinker_c_handle;

enum yukilinker_c_backend { YUKILINKER_C_BACKEND_PRIVATE = 1, YUKILINKER_C_BACKEND_SYSTEM = 2 };
enum yukilinker_c_execution_state {
  YUKILINKER_C_EXECUTION_NOT_STARTED,
  YUKILINKER_C_EXECUTION_STARTED,
  YUKILINKER_C_EXECUTION_UNKNOWN
};
enum yukilinker_c_reference_state {
  YUKILINKER_C_REFERENCE_NONE,
  YUKILINKER_C_REFERENCE_OWNED,
  YUKILINKER_C_REFERENCE_RELEASED,
  YUKILINKER_C_REFERENCE_UNKNOWN
};
enum yukilinker_c_handle_state {
  YUKILINKER_C_HANDLE_OPENING,
  YUKILINKER_C_HANDLE_READY,
  YUKILINKER_C_HANDLE_CLOSING,
  YUKILINKER_C_HANDLE_RETAINED,
  YUKILINKER_C_HANDLE_UNCERTAIN
};
enum yukilinker_c_retention_reason {
  YUKILINKER_C_RETENTION_PRIVATE_HOLD = 1U << 0,
  YUKILINKER_C_RETENTION_CLEANUP = 1U << 1,
  YUKILINKER_C_RETENTION_ABANDONED = 1U << 2,
  YUKILINKER_C_RETENTION_FORK = 1U << 3,
  YUKILINKER_C_RETENTION_SYSTEM_UNKNOWN = 1U << 4
};
enum yukilinker_c_backend_capability {
  YUKILINKER_C_PRIVATE_NAMESPACE = 1U << 0,
  YUKILINKER_C_PRIVATE_TLS = 1U << 1,
  YUKILINKER_C_SYSTEM_TLS = 1U << 2,
  YUKILINKER_C_SHARED_CXX = 1U << 3,
  YUKILINKER_C_CROSS_DSO_CFI = 1U << 4
};

/* Fixed output layouts, always written whole. A SYSTEM call may execute code even
 * if it fails: execution is UNKNOWN, never inferred from dlerror. The copied text
 * is truncated to 511 bytes. Hosts must preserve a consumed dlsym error for their
 * system_dlerror callback; otherwise the runtime supplies a generic message. */
typedef struct yukilinker_c_error {
  uint32_t size;
  uint32_t execution;
  yukilinker_c_error_detail detail;
  char system_message[512];
} yukilinker_c_error;

typedef struct yukilinker_c_open_options {
  uint32_t size;
  uint32_t backend;
  uint32_t flags;
  uint32_t file_backed;
  /* PRIVATE: select at most one context/namespace; both null select the default.
   * SYSTEM rejects either scope, file_backed, force_load and trusted_runtime. */
  yukilinker_c_context* context;
  yukilinker_c_namespace* namespace_scope;
  const char* origin;
  uint32_t force_load;
  uint32_t trusted_runtime;
} yukilinker_c_open_options;

typedef struct yukilinker_c_handle_info {
  uint32_t size;
  uint32_t backend;
  yukilinker_c_handle handle;
  uint32_t state;
  uint32_t reference;
  uint32_t retention_reasons;
  uint32_t private_nodelete_reasons;
  uint64_t capabilities;
  yukilinker_c_image_id private_image;
  uint32_t pending_operations;
  /* Only PRIVATE can confirm this. SYSTEM releasing a reference says nothing
   * about Bionic's mapping, TLS destructors, NODELETE or other users. */
  uint32_t private_mapping_unmapped;
} yukilinker_c_handle_info;

typedef struct yukilinker_c_lookup_result {
  void* address;
  uint32_t found;
  yukilinker_c_error error;
} yukilinker_c_lookup_result;

typedef struct yukilinker_c_close_result {
  yukilinker_c_handle_info info;
  yukilinker_c_error error;
  uint32_t handle_valid;
} yukilinker_c_close_result;

typedef struct yukilinker_c_runtime_stats {
  uint32_t size;
  uint32_t handle_limit;
  uint64_t handles;
  uint64_t uncertain_handles;
  uint64_t pending_operations;
  uint64_t handle_bytes;
  uint64_t stub_budget_bytes;
  uint64_t stub_mapped_bytes;
  uint64_t stub_used_slots;
  uint64_t stub_capacity;
  uint64_t images;
  uint64_t mapped_bytes;
  uint64_t arena_bytes;
  uint64_t peak_arena_bytes;
  uint64_t tls_blocks;
  uint64_t thread_destructors;
} yukilinker_c_runtime_stats;

#define YUKILINKER_C_DEFAULT_HANDLE_LIMIT 1024U

/* Stage-one ABI, compatible with YukiZygisk's original Core handoff.
 * The caller system-loads libyloaderXX.so and transfers its initial dlopen
 * reference on a successful Core adoption. Do not link the Core against this
 * library. It must contain its own runtime (archive or source integration).
 *
 * A call admitted by the single-flight guard consumes core_fd. Before entry,
 * the loader owns the early packet and its descriptors; at zygisk_core_entry
 * that ownership passes to the Core. Invalid/busy calls leave inputs untouched.
 * Success tail-calls the Core finalizer, which dlcloses the transferred loader
 * reference. No return address may remain in the loader. Failure sets errno;
 * a call that ran Core code or could not clean up retains its mapping/state.
 * Reentry and a fork inheriting unfinished work are refused (EBUSY).
 */
void yuki_bootstrap(int core_fd, int early_packet_fd_plus1);
#if defined(__arm__)
void yuki_bootstrap_tango(uint32_t got, uint32_t original, uint32_t stub, int core_fd,
                          int early_packet_fd_plus1);
#endif
#define YUKILINKER_C_TANGO_BOOTSTRAP_SIZE 224U

/* Same-process native extension. No pointers in origin refer to the temporary
 * injector. A successful bind consumes precisely loader_reference; refusal
 * consumes nothing. The entry consumes descriptors under the normal bootstrap
 * admission rules and tail-calls the same Core finalizer. */
#define YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION 1U
typedef struct yukilinker_c_native_origin {
  uint32_t size, version, process, generation;
  uint64_t linker_base, libc_base, libc_anchor;
  char libc_path[256];
} yukilinker_c_native_origin;
#define YUKILINKER_C_NATIVE_LOADER_RETAINED 1U
#define YUKILINKER_C_NATIVE_LOADER_ADOPTED 2U
#define YUKILINKER_C_NATIVE_LOADER_RETURNED 4U
typedef struct yukilinker_c_native_completion {
  uint32_t size, version, process, generation;
  int32_t error;
  uint32_t ownership;
} yukilinker_c_native_completion;
typedef struct yukilinker_c_native_binding {
  uint32_t size, version;
  void* loader_reference;
  yukilinker_c_native_origin origin;
  /* Borrowed through entry only. Lives outside the temporary resolver and the
   * loader. RETURNED permits exactly one external dlclose after entry returns;
   * RETAINED/ADOPTED never permit the injector to release that reference. */
  yukilinker_c_native_completion* completion;
} yukilinker_c_native_binding;
/* The error buffer belongs to the Core. The callback borrows request/fd and
 * returns one owned Bionic reference. It never retains any request storage. */
typedef void* (*yukilinker_c_native_open)(void*, const yukilinker_c_host_dlopen_request*, char*,
                                          size_t);
typedef struct yukilinker_c_native_services {
  uint32_t size, version;
  yukilinker_c_native_origin origin;
  void* context;
  yukilinker_c_native_open open;
} yukilinker_c_native_services;
typedef bool (*yukilinker_c_core_bind_native)(const yukilinker_c_native_services*);
bool yuki_bootstrap_bind_native_v1(const yukilinker_c_native_binding*);
void yuki_bootstrap_native_v1(int core_fd, int early_packet_fd_plus1);

/* Required Core exports. The name is borrowed for the entry call; the Core
 * copies any data it retains. loader_anchor identifies the stage-one mapping.
 * The Core must prove that all persistent code/data/TLS/callback references
 * survive loader unload before handoff_ready returns true. Dependencies are
 * the exact dlopen references stage one acquired; take_dependencies copies
 * the list and takes all references only on true. finish_handoff commits once.
 * finalizers run in the Core, outside stage one's stack, and release the
 * adopted loader reference with system dlclose. They must not call it again.
 */
typedef void (*yukilinker_c_core_entry)(const char*, void*, void*, void*, int);
typedef bool (*yukilinker_c_core_adopt)(uintptr_t, size_t, void*);
typedef bool (*yukilinker_c_core_ready)(void);
typedef bool (*yukilinker_c_core_take_dependencies)(void* const*, size_t);
typedef bool (*yukilinker_c_core_finish_handoff)(bool);
typedef void (*yukilinker_c_core_finalize)(int, int);
typedef bool (*yukilinker_c_core_set_tango)(uint32_t, uint32_t);
typedef void (*yukilinker_c_core_finalize_tango)(uint32_t, uint32_t, uint32_t, int, int);

/* Implement these in the Core. They are the only public handoff symbols;
 * yukilinker_c_* runtime functions remain bound within that Core. */
#if defined(__GNUC__)
#define YUKILINKER_CORE_EXPORT __attribute__((visibility("default")))
#else
#define YUKILINKER_CORE_EXPORT
#endif
YUKILINKER_CORE_EXPORT void zygisk_core_entry(const char*, void*, void*, void*, int);
YUKILINKER_CORE_EXPORT bool zygisk_adopt_anonymous_image(uintptr_t, size_t, void*);
YUKILINKER_CORE_EXPORT bool zygisk_bootstrap_handoff_ready(void);
YUKILINKER_CORE_EXPORT bool zygisk_bootstrap_take_dependencies(void* const*, size_t);
YUKILINKER_CORE_EXPORT bool zygisk_bootstrap_finish_handoff(bool);
YUKILINKER_CORE_EXPORT bool zygisk_bootstrap_bind_native_v1(const yukilinker_c_native_services*);
YUKILINKER_CORE_EXPORT void zygisk_finalize_loader(int, int);
#if defined(__arm__)
YUKILINKER_CORE_EXPORT bool zygisk_set_tango_stub(uint32_t, uint32_t);
YUKILINKER_CORE_EXPORT void zygisk_finalize_loader_tango(uint32_t, uint32_t, uint32_t, int, int);
#endif

/* Frozen transport from YukiZygisk 509417ca, kernel/uapi/yukizygisk.h
 * (Apache-2.0, Rouyashiki <https://rouyashiki.org>). The SDK only transfers/cleans these
 * descriptors; the Core owns endpoint communication and configuration. */
#define YUKILINKER_C_EARLY_PACKET_MAGIC UINT32_C(0x59504e5a)
#define YUKILINKER_C_EARLY_PACKET_VERSION 3U
#define YUKILINKER_C_EARLY_PACKET_MAX_ENTRIES 64U
typedef struct yukilinker_c_early_packet_header {
  uint32_t magic;
  uint16_t version, header_size, entry_size, load_flags;
  uint32_t count;
  int32_t endpoint_fd;
  uint32_t reserved;
} yukilinker_c_early_packet_header;
typedef struct yukilinker_c_early_packet_entry {
  uint8_t target_type, flags;
  uint16_t reserved;
  char module_id[64], target[256], lib_path[512];
  int32_t fd;
  uint32_t tail_reserved;
} yukilinker_c_early_packet_entry;

enum yukilinker_c_feature {
  YUKILINKER_C_FEATURE_PACKED_RELOCS = 1,
  YUKILINKER_C_FEATURE_SYMBOL_VERSIONS = 2,
  YUKILINKER_C_FEATURE_PRIVATE_DEPENDENCIES = 3,
  YUKILINKER_C_FEATURE_DYNAMIC_TLS = 4,
  YUKILINKER_C_FEATURE_SNAPSHOTS = 5,
  YUKILINKER_C_FEATURE_IMAGE_LEASES = 6,
  YUKILINKER_C_FEATURE_EVENTS = 7,
  YUKILINKER_C_FEATURE_NAMESPACES = 8,
  YUKILINKER_C_FEATURE_MODULE_PHDR = 9,
  YUKILINKER_C_FEATURE_DLEXT_FD_OFFSET = 10,
  YUKILINKER_C_FEATURE_DLEXT_RESERVED_ROOT = 11,
  YUKILINKER_C_FEATURE_DLEXT_RELRO_ROOT = 12,
  YUKILINKER_C_FEATURE_MAPPING_STATE = 13,
  YUKILINKER_C_FEATURE_TYPED_LEASES = 14,
  YUKILINKER_C_FEATURE_RUNTIME_OWNER = 15,
  YUKILINKER_C_FEATURE_COPIED_VIEWS = 16,
  YUKILINKER_C_FEATURE_HOST_SERVICES = 17,
  YUKILINKER_C_FEATURE_HOST_BIND = 18,
  YUKILINKER_C_FEATURE_IMAGE_DETAIL = 19,
  YUKILINKER_C_FEATURE_NAMESPACE_LINKS = 20,
  YUKILINKER_C_FEATURE_HOST_IMPORTS = 21,
  YUKILINKER_C_FEATURE_HOST_IMPORT_REFUSAL = 22,
  YUKILINKER_C_FEATURE_LOAD_WAITS = 23,
  YUKILINKER_C_FEATURE_SYSTEM_BACKEND = 24,
  YUKILINKER_C_FEATURE_GENERATION_HANDLES = 25,
  YUKILINKER_C_FEATURE_RESOURCE_LIMITS = 26,
  YUKILINKER_C_FEATURE_TRUSTED_HANDOFF = 27,
  YUKILINKER_C_FEATURE_CONTROLLED_FORK = 28
};

/* Pure queries: no allocation, runtime state, TLS keys or atfork registration. */
YUKILINKER_C_API uint32_t yukilinker_c_abi_version(void);

YUKILINKER_C_API uint32_t yukilinker_c_has_feature(uint32_t feature);

/* One owner per Core; services must be valid and are copied. The lifecycle is terminal.
 * Host callbacks must outlive the owner and must not throw across the C boundary. */
YUKILINKER_C_API yukilinker_c_runtime* yukilinker_c_runtime_create(
    const yukilinker_c_host_services* services);

/* Returns 1 on complete retirement, 0 on refusal/partial cleanup; retry is permitted.
 * Stop all calls/TLS/callback users before destruction or unmapping the containing Core. */
YUKILINKER_C_API uint32_t yukilinker_c_runtime_destroy(yukilinker_c_runtime* runtime);

/* Returns 1 on shutdown, 0 otherwise. A 1 with cleanup_incomplete reports a leaked
 * system dependency reference; proceed with runtime_destroy. A refused shutdown changes
 * nothing; after the retirement boundary, RETRY/IO_ERROR resumes without repeating code. */
YUKILINKER_C_API uint32_t yukilinker_c_try_shutdown(yukilinker_c_runtime* runtime,
                                                    yukilinker_c_error_detail* error);

/* Null restores the Core-local defaults; refuses while a module or operation holds the host. */
YUKILINKER_C_API uint32_t yukilinker_c_host_services_bind(
    yukilinker_c_runtime* runtime, const yukilinker_c_host_services* services);

/* Hard limits; handle_limit is 1..65536, fixed after first allocation; stub budget includes RX/RW
 * pages. */
YUKILINKER_C_API uint32_t yukilinker_c_set_limits(yukilinker_c_runtime* runtime,
                                                  uint32_t handle_limit, uint64_t stub_bytes);

YUKILINKER_C_API uint32_t yukilinker_c_query_stats(yukilinker_c_runtime* runtime,
                                                   yukilinker_c_runtime_stats* output);

/* Provider success transfers its fd to the runtime. Search path is copied; provider/user
 * remain valid until context destruction. Destruction refuses active references and seals
 * the scope once retirement starts. A failed retirement can be resumed. */
YUKILINKER_C_API yukilinker_c_context* yukilinker_c_context_create(
    yukilinker_c_runtime* runtime, const char* search_path,
    yukilinker_c_dependency_provider provider, void* user);

/* Boolean: 1 destroyed, 0 retained. No callback may keep executing in a retired scope. */
YUKILINKER_C_API uint32_t yukilinker_c_context_destroy(yukilinker_c_runtime* runtime,
                                                       yukilinker_c_context* context);

YUKILINKER_C_API yukilinker_c_namespace* yukilinker_c_namespace_create(
    yukilinker_c_runtime* runtime, yukilinker_c_context* parent);

YUKILINKER_C_API uint32_t yukilinker_c_namespace_destroy(yukilinker_c_runtime* runtime,
                                                         yukilinker_c_namespace* scope);

YUKILINKER_C_API uint64_t yukilinker_c_namespace_id(yukilinker_c_runtime* runtime,
                                                    yukilinker_c_namespace* scope);

YUKILINKER_C_API uint32_t yukilinker_c_namespace_link(yukilinker_c_runtime* runtime,
                                                      yukilinker_c_namespace* from,
                                                      yukilinker_c_namespace* to, uint32_t flags,
                                                      const char* shared_sonames);

YUKILINKER_C_API uint32_t yukilinker_c_namespace_links(yukilinker_c_runtime* runtime,
                                                       yukilinker_c_namespace* from,
                                                       yukilinker_c_namespace_link_record* records,
                                                       size_t capacity, size_t* count);

YUKILINKER_C_API uint32_t
yukilinker_c_namespace_host_import(yukilinker_c_runtime* runtime, yukilinker_c_namespace* scope,
                                   const yukilinker_c_host_import* import);

YUKILINKER_C_API uint32_t yukilinker_c_namespace_host_imports(
    yukilinker_c_runtime* runtime, yukilinker_c_namespace* scope,
    yukilinker_c_host_import_record* records, size_t capacity, size_t* count);

YUKILINKER_C_API uint32_t yukilinker_c_query_host_import_refusal(
    yukilinker_c_runtime* runtime, yukilinker_c_host_import_refusal* refusal);

YUKILINKER_C_API uint32_t yukilinker_c_wait_timeout(yukilinker_c_runtime* runtime,
                                                    uint64_t nanoseconds);

YUKILINKER_C_API uint32_t yukilinker_c_load_waits(yukilinker_c_runtime* runtime,
                                                  yukilinker_c_load_wait* records, size_t capacity,
                                                  size_t* count);

/* Each successful open owns one distinct generation handle. fd is borrowed, offset zero.
 * PRIVATE supports trusted_runtime for explicit host handoff. SYSTEM calls may execute
 * even on failure: UNKNOWN must not be interpreted as safe rollback. */
YUKILINKER_C_API yukilinker_c_handle
yukilinker_c_open_path(yukilinker_c_runtime* runtime, const char* path,
                       const yukilinker_c_open_options* options, yukilinker_c_error* error);

/* Each successful open owns one distinct generation handle. fd is borrowed, offset zero.
 * PRIVATE supports trusted_runtime for explicit host handoff. SYSTEM calls may execute
 * even on failure: UNKNOWN must not be interpreted as safe rollback. */
YUKILINKER_C_API yukilinker_c_handle yukilinker_c_open_fd(yukilinker_c_runtime* runtime, int fd,
                                                          const yukilinker_c_open_options* options,
                                                          yukilinker_c_error* error);

/* found distinguishes a valid null-valued symbol from a miss. Returned addresses are borrowed.
 * Hold an execution lease or otherwise prevent retirement for the full duration of calls. */
YUKILINKER_C_API yukilinker_c_lookup_result yukilinker_c_lookup(yukilinker_c_runtime* runtime,
                                                                yukilinker_c_handle handle,
                                                                const char* name,
                                                                const char* version);

YUKILINKER_C_API uint32_t yukilinker_c_query(yukilinker_c_runtime* runtime,
                                             yukilinker_c_handle handle,
                                             yukilinker_c_handle_info* output);

/* Includes retained/uncertain handles, even when an open never returned. count is total;
 * BUFFER_TOO_SMALL writes a prefix. Unknown ownership prevents later opens/lookups/closes. */
YUKILINKER_C_API uint32_t yukilinker_c_handles(yukilinker_c_runtime* runtime,
                                               yukilinker_c_handle_info* output, size_t capacity,
                                               size_t* count);

/* A retained PRIVATE handle retries collection without releasing another reference.
 * SYSTEM success only releases its reference; it does not promise an unmap. Use
 * handle_valid, reference, retention reasons and error together; a failure may retain ownership. */
YUKILINKER_C_API yukilinker_c_close_result yukilinker_c_close(yukilinker_c_runtime* runtime,
                                                              yukilinker_c_handle handle);

/* Only forgets PRIVATE wrapper metadata after its reference was released; no unmap claim. */
YUKILINKER_C_API uint32_t yukilinker_c_forget(yukilinker_c_runtime* runtime,
                                              yukilinker_c_handle handle);

/* Error code: OK transfers a Ready trusted image mapping to the caller and invalidates
 * the handle, leaving the mapping intact. Refuses while used/pinned/referenced elsewhere.
 * Dependencies are closed by this operation; bootstrap uses its separate dependency-transfer
 * protocol. The receiving host must ultimately retire the mapping and all its callbacks. */
YUKILINKER_C_API uint32_t yukilinker_c_release_host(yukilinker_c_runtime* runtime,
                                                    yukilinker_c_handle handle);

/* PRIVATE image record; SYSTEM reports UNSUPPORTED. No raw Bionic/private handle escapes. */
YUKILINKER_C_API uint32_t yukilinker_c_handle_image(yukilinker_c_runtime* runtime,
                                                    yukilinker_c_handle handle,
                                                    yukilinker_c_image_record* output);

/* Select at most one context/namespace. With neither specified, select all visible images.
 * Snapshots pin their images until release. Program header capacities/required values are bytes. */
YUKILINKER_C_API uint32_t yukilinker_c_snapshot_acquire(yukilinker_c_runtime* runtime,
                                                        yukilinker_c_context* context,
                                                        yukilinker_c_namespace* scope,
                                                        yukilinker_c_snapshot* token,
                                                        size_t* count);

YUKILINKER_C_API uint32_t yukilinker_c_snapshot_image(yukilinker_c_runtime* runtime,
                                                      yukilinker_c_snapshot token, size_t index,
                                                      yukilinker_c_image_record* output);

YUKILINKER_C_API uint32_t yukilinker_c_snapshot_program_headers(yukilinker_c_runtime* runtime,
                                                                yukilinker_c_snapshot token,
                                                                size_t index, void* headers,
                                                                size_t capacity, size_t* required);

YUKILINKER_C_API uint32_t yukilinker_c_snapshot_mapping(yukilinker_c_runtime* runtime,
                                                        yukilinker_c_snapshot token, size_t index,
                                                        yukilinker_c_mapping_record* output);

/* Release consumes the token even if subsequent collection reports IO_ERROR. The in/out
 * value is cleared exactly when consumed; inspect it before retrying a failed release. */
YUKILINKER_C_API uint32_t yukilinker_c_snapshot_release(yukilinker_c_runtime* runtime,
                                                        yukilinker_c_snapshot* token);

YUKILINKER_C_API uint32_t yukilinker_c_image_acquire(yukilinker_c_runtime* runtime,
                                                     yukilinker_c_image_id id, uint32_t kind,
                                                     yukilinker_c_lease* lease);

/* Like snapshot_release, clears the in/out token once consumed, including cleanup failure. */
YUKILINKER_C_API uint32_t yukilinker_c_image_release(yukilinker_c_runtime* runtime,
                                                     yukilinker_c_lease* lease);

YUKILINKER_C_API uint32_t yukilinker_c_image_query(yukilinker_c_runtime* runtime,
                                                   yukilinker_c_image_id id,
                                                   yukilinker_c_image_record* record);

YUKILINKER_C_API uint32_t yukilinker_c_query_image_detail(yukilinker_c_runtime* runtime,
                                                          yukilinker_c_image_id id,
                                                          yukilinker_c_image_detail* detail);

YUKILINKER_C_API uint32_t yukilinker_c_image_dependencies(yukilinker_c_runtime* runtime,
                                                          yukilinker_c_image_id id,
                                                          yukilinker_c_dependency_record* records,
                                                          size_t capacity, size_t* count);

YUKILINKER_C_API uint32_t yukilinker_c_image_program_headers(yukilinker_c_runtime* runtime,
                                                             yukilinker_c_image_id id,
                                                             void* headers, size_t capacity,
                                                             size_t* required);

/* RETRY reports the oldest available cursor after ring overflow. BUFFER_TOO_SMALL reports
 * required count and copies nothing; retry with adequate capacity. */
YUKILINKER_C_API uint32_t yukilinker_c_read_events(yukilinker_c_runtime* runtime, uint64_t cursor,
                                                   yukilinker_c_runtime_event* events,
                                                   size_t capacity, size_t* written,
                                                   uint64_t* next_cursor);

/* Boolean result; name/address are borrowed only for the duration of the surrounding
 * module/lease lifetime. Prefer copied image records for durable observations. */
YUKILINKER_C_API uint32_t yukilinker_c_query_address(yukilinker_c_runtime* runtime,
                                                     const void* address,
                                                     yukilinker_c_image_info* output);

/* Select at most one context/namespace. With neither specified, select all visible images.
 * Returns the visitor's nonzero stop value, 0 at the end, or -1 on invalid arguments,
 * an inactive owner, invalid scope or inability to acquire the iteration snapshot. */
YUKILINKER_C_API int yukilinker_c_iterate_images(yukilinker_c_runtime* runtime,
                                                 yukilinker_c_context* context,
                                                 yukilinker_c_namespace* scope,
                                                 yukilinker_c_image_visitor visitor, void* user);

/* Controlled fork brackets; nest only on the same thread and pair with after_fork.
 * These supplement the runtime atfork handlers and cannot restart a retired runtime. */
YUKILINKER_C_API void yukilinker_c_prepare_fork(yukilinker_c_runtime* runtime);

/* Controlled fork brackets; nest only on the same thread and pair with after_fork.
 * These supplement the runtime atfork handlers and cannot restart a retired runtime. */
YUKILINKER_C_API void yukilinker_c_after_fork(yukilinker_c_runtime* runtime, int child);

/* Best-effort host diagnostic only; does not release ownership or execute private module code. */
YUKILINKER_C_API void yukilinker_c_report_unreleased(yukilinker_c_runtime* runtime,
                                                     const char* object);

#ifdef __cplusplus
}
#endif
#endif /* YUKILINKER_H */
