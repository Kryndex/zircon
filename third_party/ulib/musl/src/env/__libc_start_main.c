#include "libc.h"
#include "pthread_impl.h"
#include <elf.h>
#include <stdatomic.h>
#include <string.h>

#include <magenta/internal.h>
#include <magenta/syscalls.h>
#include <runtime/message.h>
#include <runtime/processargs.h>
#include <runtime/thread.h>

// hook for extension libraries to init
void __libc_extensions_init(uint32_t handle_count,
                            mx_handle_t handle[],
                            uint32_t handle_info[]) __attribute__((weak));

struct start_params {
    uint32_t argc, nhandles;
    char** argv;
    mx_handle_t* handles;
    uint32_t* handle_info;
    int (*main)(int, char**, char**);
};

// This gets called via inline assembly below, after switching onto
// the newly-allocated (safe) stack.
static _Noreturn void start_main(const struct start_params*)
    __asm__("start_main") __attribute__((used));
static void start_main(const struct start_params* p) {
    // Allow companion libraries a chance to poke at this.
    if (&__libc_extensions_init != NULL)
        __libc_extensions_init(p->nhandles, p->handles, p->handle_info);

    // Run static constructors et al.
    __libc_start_init();

    // Pass control to the application.
    exit((*p->main)(p->argc, p->argv, __environ));
}

_Noreturn void __libc_start_main(void* arg, int (*main)(int, char**, char**)) {
    // Initialize stack-protector canary value first thing.
    size_t actual;
    mx_status_t status = mx_cprng_draw(&__stack_chk_guard,
                                       sizeof(__stack_chk_guard), &actual);
    if (status != NO_ERROR || actual != sizeof(__stack_chk_guard))
        __builtin_trap();

    // extract process startup information from channel in arg
    mx_handle_t bootstrap = (uintptr_t)arg;

    struct start_params p = { .main = main };
    uint32_t nbytes;
    status = mxr_message_size(bootstrap, &nbytes, &p.nhandles);
    if (status != NO_ERROR)
        nbytes = p.nhandles = 0;

    MXR_PROCESSARGS_BUFFER(buffer, nbytes);
    mx_handle_t handles[p.nhandles];
    p.handles = handles;
    mx_proc_args_t* procargs = NULL;
    if (status == NO_ERROR)
        status = mxr_processargs_read(bootstrap, buffer, nbytes,
                                      handles, p.nhandles,
                                      &procargs, &p.handle_info);

    uint32_t envc = 0;
    if (status == NO_ERROR) {
        p.argc = procargs->args_num;
        envc = procargs->environ_num;
    }

    // Use a single contiguous buffer for argv and envp, with two
    // extra words of terminator on the end.  In traditional Unix
    // process startup, the stack contains argv followed immediately
    // by envp and that's followed immediately by the auxiliary vector
    // (auxv), which is in two-word pairs and terminated by zero
    // words.  Some crufty programs might assume some of that layout,
    // and it costs us nothing to stay consistent with it here.
    char* args_and_environ[p.argc + 1 + envc + 1 + 2];
    p.argv = &args_and_environ[0];
    __environ = &args_and_environ[p.argc + 1];
    char** dummy_auxv = &args_and_environ[p.argc + 1 + envc + 1];
    dummy_auxv[0] = dummy_auxv[1] = 0;

    if (status == NO_ERROR)
        status = mxr_processargs_strings(buffer, nbytes, p.argv, __environ);
    if (status != NO_ERROR) {
        p.argc = 0;
        p.argv = __environ = NULL;
    }

    // Find the handles we're interested in among what we were given.
    mx_handle_t main_thread_handle = MX_HANDLE_INVALID;
    for (uint32_t i = 0; i < p.nhandles; ++i) {
        switch (MX_HND_INFO_TYPE(p.handle_info[i])) {
        case MX_HND_TYPE_PROC_SELF:
            // The handle will have been installed already by dynamic
            // linker startup, but now we have another one.  They
            // should of course be handles to the same process, but
            // just for cleanliness switch to the "main" one.
            if (__magenta_process_self != MX_HANDLE_INVALID)
                _mx_handle_close(__magenta_process_self);
            __magenta_process_self = handles[i];
            handles[i] = MX_HANDLE_INVALID;
            p.handle_info[i] = 0;
            break;

        case MX_HND_TYPE_JOB:
            // The default job provided to the process to use for
            // creation of additional processes.  It may or may not
            // be the job this process is a child of.  It may not
            // be provided at all.
            if (__magenta_job_default != MX_HANDLE_INVALID)
                _mx_handle_close(__magenta_job_default);
            __magenta_job_default = handles[i];
            handles[i] = MX_HANDLE_INVALID;
            p.handle_info[i] = 0;
            break;

        case MX_HND_TYPE_VMAR_ROOT:
            // As above for PROC_SELF
            if (__magenta_vmar_root_self != MX_HANDLE_INVALID)
                _mx_handle_close(__magenta_vmar_root_self);
            __magenta_vmar_root_self = handles[i];
            handles[i] = MX_HANDLE_INVALID;
            p.handle_info[i] = 0;
            break;

        case MX_HND_TYPE_THREAD_SELF:
            main_thread_handle = handles[i];
            handles[i] = MX_HANDLE_INVALID;
            p.handle_info[i] = 0;
            break;
        }
    }

    atomic_store(&libc.thread_count, 1);

    // This consumes the thread handle and sets up the thread pointer.
    pthread_t td = __init_main_thread(main_thread_handle);

    // Switch to the allocated stack and call start_main(&p) there.
    // The original stack stays around just to hold argv et al.
    // The new stack is whole pages, so it's sufficiently aligned.

#ifdef __x86_64__
    // The x86-64 ABI requires %rsp % 16 = 8 on entry.  The zero word
    // at (%rsp) serves as the return address for the outermost frame.
    __asm__("lea -8(%[base], %[len], 1), %%rsp\n"
            "jmp start_main\n"
            "# Target receives %[arg]" : :
            [base]"r"(td->safe_stack.iov_base),
            [len]"r"(td->safe_stack.iov_len),
            [arg]"D"(&p));
#elif defined(__aarch64__)
    __asm__("add sp, %[base], %[len]\n"
            "mov x0, %[arg]\n"
            "b start_main" : :
            [base]"r"(td->safe_stack.iov_base),
            [len]"r"(td->safe_stack.iov_len),
            [arg]"r"(&p));
#else
#error what architecture?
#endif

    __builtin_unreachable();
}
