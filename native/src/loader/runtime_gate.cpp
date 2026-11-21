#include "runtime_gate.h"

#include "log.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__LP64__)
#define MOCHA_ELF_R_SYM ELF64_R_SYM
#define MOCHA_ELF_R_TYPE ELF64_R_TYPE
using MochaElfDyn = Elf64_Dyn;
using MochaElfSym = Elf64_Sym;
using MochaElfRela = Elf64_Rela;
using MochaElfRel = Elf64_Rel;
#else
#define MOCHA_ELF_R_SYM ELF32_R_SYM
#define MOCHA_ELF_R_TYPE ELF32_R_TYPE
using MochaElfDyn = Elf32_Dyn;
using MochaElfSym = Elf32_Sym;
using MochaElfRela = Elf32_Rela;
using MochaElfRel = Elf32_Rel;
#endif

#if defined(__aarch64__)
constexpr unsigned kJumpSlot = R_AARCH64_JUMP_SLOT;
constexpr unsigned kGlobDat = R_AARCH64_GLOB_DAT;
#elif defined(__x86_64__)
constexpr unsigned kJumpSlot = R_X86_64_JUMP_SLOT;
constexpr unsigned kGlobDat = R_X86_64_GLOB_DAT;
#elif defined(__arm__)
constexpr unsigned kJumpSlot = R_ARM_JUMP_SLOT;
constexpr unsigned kGlobDat = R_ARM_GLOB_DAT;
#elif defined(__i386__)
constexpr unsigned kJumpSlot = R_386_JMP_SLOT;
constexpr unsigned kGlobDat = R_386_GLOB_DAT;
#else
constexpr unsigned kJumpSlot = 0;
constexpr unsigned kGlobDat = 0;
#endif

namespace mocha {
namespace {

using DlsymFn = void* (*)(void*, const char*);
using Il2CppInitFn = void* (*)(const void*);

std::mutex g_install_mutex;
std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
bool g_init_complete = false;
std::atomic<bool> g_installed {false};
std::atomic<void (*)()> g_ready_callback {nullptr};
std::atomic<bool> g_ready_ran {false};
std::atomic<DlsymFn> g_real_dlsym {nullptr};
std::atomic<Il2CppInitFn> g_real_init {nullptr};
std::atomic<Il2CppInitFn> g_real_init_utf16 {nullptr};

struct UnityImage {
    uintptr_t bias = 0;
    const ElfW(Phdr)* phdrs = nullptr;
    size_t phnum = 0;
    bool found = false;
};

int find_unity(struct dl_phdr_info* info, size_t, void* data) {
    auto* image = static_cast<UnityImage*>(data);
    if (!info->dlpi_name || info->dlpi_name[0] == '\0') {
        return 0;
    }
    const char* slash = std::strrchr(info->dlpi_name, '/');
    const char* name = slash ? slash + 1 : info->dlpi_name;
    if (std::strcmp(name, "libunity.so") != 0) {
        return 0;
    }
    image->bias = static_cast<uintptr_t>(info->dlpi_addr);
    image->phdrs = reinterpret_cast<const ElfW(Phdr)*>(info->dlpi_phdr);
    image->phnum = info->dlpi_phnum;
    image->found = true;
    return 1;
}

int prot_for_address(uintptr_t address) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) {
        return -1;
    }
    char line[512];
    int prot = -1;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[5] = {};
        if (std::sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) {
            continue;
        }
        if (address < start || address >= end) {
            continue;
        }
        prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;
        if (perms[2] == 'x') prot |= PROT_EXEC;
        break;
    }
    std::fclose(maps);
    return prot;
}

bool replace_slot(uintptr_t slot, void* replacement, void*& original) {
    auto* target = reinterpret_cast<void**>(slot);
    original = *target;

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return false;
    }
    const uintptr_t page_mask = static_cast<uintptr_t>(page_size) - 1;
    const uintptr_t start = slot & ~page_mask;
    const uintptr_t end = (slot + sizeof(void*) + page_mask) & ~page_mask;

    const int prot = prot_for_address(slot);
    if (prot < 0) {
        return false;
    }
    if (mprotect(reinterpret_cast<void*>(start), end - start, prot | PROT_WRITE) != 0) {
        return false;
    }
    *target = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + sizeof(void*));
    mprotect(reinterpret_cast<void*>(start), end - start, prot);
    return true;
}

void mark_init_complete(const char* which, void* domain) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        if (!g_init_complete) {
            g_init_complete = true;
            first = true;
            log_printf(MOCHA_LOG_INFO, "%s returned the IL2CPP domain (%p)", which, domain);
        }
    }
    if (!first) {
        return;
    }
    g_gate_cv.notify_all();

    auto callback = g_ready_callback.load();
    if (!callback) {
        log_write(MOCHA_LOG_WARN, "no runtime-ready callback was registered");
        return;
    }
    if (g_ready_ran.exchange(true)) {
        return;
    }
    log_printf(MOCHA_LOG_INFO, "running mod initialization on the %s thread", which);
    callback();
    log_write(MOCHA_LOG_INFO, "mod initialization returned to the engine");
}

void* mocha_il2cpp_init(const void* argument) {
    Il2CppInitFn real = g_real_init.load();
    if (!real) {
        log_write(MOCHA_LOG_ERROR, "il2cpp_init was invoked without a resolved symbol");
        return nullptr;
    }
    void* domain = real(argument);
    if (domain) {
        mark_init_complete("il2cpp_init", domain);
    } else {
        log_write(MOCHA_LOG_ERROR, "il2cpp_init returned null");
    }
    return domain;
}

void* mocha_il2cpp_init_utf16(const void* argument) {
    Il2CppInitFn real = g_real_init_utf16.load();
    if (!real) {
        log_write(MOCHA_LOG_ERROR, "il2cpp_init_utf16 was invoked without a resolved symbol");
        return nullptr;
    }
    void* domain = real(argument);
    if (domain) {
        mark_init_complete("il2cpp_init_utf16", domain);
    } else {
        log_write(MOCHA_LOG_ERROR, "il2cpp_init_utf16 returned null");
    }
    return domain;
}

void* mocha_dlsym(void* handle, const char* symbol) {
    DlsymFn real = g_real_dlsym.load();
    if (!real) {
        return nullptr;
    }
    void* resolved = real(handle, symbol);
    if (!resolved || !symbol) {
        return resolved;
    }
    if (std::strcmp(symbol, "il2cpp_init") == 0) {
        g_real_init.store(reinterpret_cast<Il2CppInitFn>(resolved));
        log_write(MOCHA_LOG_INFO, "intercepted libunity il2cpp_init lookup");
        return reinterpret_cast<void*>(&mocha_il2cpp_init);
    }
    if (std::strcmp(symbol, "il2cpp_init_utf16") == 0) {
        g_real_init_utf16.store(reinterpret_cast<Il2CppInitFn>(resolved));
        log_write(MOCHA_LOG_INFO, "intercepted libunity il2cpp_init_utf16 lookup");
        return reinterpret_cast<void*>(&mocha_il2cpp_init_utf16);
    }
    return resolved;
}

const char* symbol_name(uintptr_t strtab, size_t strsz, const MochaElfSym& symbol) {
    if (symbol.st_name >= strsz) {
        return nullptr;
    }
    return reinterpret_cast<const char*>(strtab + symbol.st_name);
}

bool is_target_relocation(unsigned type) {
    return type == kJumpSlot || type == kGlobDat;
}

struct Table {
    uintptr_t base = 0;
    const MochaElfSym* symbols = nullptr;
    uintptr_t strtab = 0;
    size_t strsz = 0;
    bool patched = false;
    void* original = nullptr;
};

bool patch_rela_table(Table& table, const MochaElfRela* entries, size_t bytes) {
    const size_t count = bytes / sizeof(MochaElfRela);
    for (size_t i = 0; i < count; ++i) {
        const MochaElfRela& entry = entries[i];
        if (!is_target_relocation(MOCHA_ELF_R_TYPE(entry.r_info))) {
            continue;
        }
        const MochaElfSym& symbol = table.symbols[MOCHA_ELF_R_SYM(entry.r_info)];
        const char* name = symbol_name(table.strtab, table.strsz, symbol);
        if (!name || std::strcmp(name, "dlsym") != 0) {
            continue;
        }
        void* original = nullptr;
        if (!replace_slot(table.base + entry.r_offset, reinterpret_cast<void*>(&mocha_dlsym),
                          original)) {
            continue;
        }
        if (!table.original && original != reinterpret_cast<void*>(&mocha_dlsym)) {
            table.original = original;
        }
        table.patched = true;
    }
    return table.patched;
}

bool patch_rel_table(Table& table, const MochaElfRel* entries, size_t bytes) {
    const size_t count = bytes / sizeof(MochaElfRel);
    for (size_t i = 0; i < count; ++i) {
        const MochaElfRel& entry = entries[i];
        if (!is_target_relocation(MOCHA_ELF_R_TYPE(entry.r_info))) {
            continue;
        }
        const MochaElfSym& symbol = table.symbols[MOCHA_ELF_R_SYM(entry.r_info)];
        const char* name = symbol_name(table.strtab, table.strsz, symbol);
        if (!name || std::strcmp(name, "dlsym") != 0) {
            continue;
        }
        void* original = nullptr;
        if (!replace_slot(table.base + entry.r_offset, reinterpret_cast<void*>(&mocha_dlsym),
                          original)) {
            continue;
        }
        if (!table.original && original != reinterpret_cast<void*>(&mocha_dlsym)) {
            table.original = original;
        }
        table.patched = true;
    }
    return table.patched;
}

} // namespace

bool install_runtime_gate(void* /*unity_handle*/, std::string& error) {
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_installed.load()) {
        return true;
    }

    UnityImage unity;
    dl_iterate_phdr(find_unity, &unity);
    if (!unity.found) {
        error = "libunity.so is not mapped";
        return false;
    }

    const MochaElfDyn* dynamic = nullptr;
    for (size_t i = 0; i < unity.phnum; ++i) {
        const ElfW(Phdr)& phdr = unity.phdrs[i];
        if (phdr.p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<const MochaElfDyn*>(unity.bias + phdr.p_vaddr);
            break;
        }
    }
    if (!dynamic) {
        error = "libunity.so has no PT_DYNAMIC segment";
        return false;
    }

    uintptr_t symtab = 0;
    uintptr_t strtab = 0;
    size_t strsz = 0;
    uintptr_t jmprel = 0;
    size_t pltrelsz = 0;
    long pltrel = 0;
    uintptr_t rela = 0;
    size_t relasz = 0;
    uintptr_t rel = 0;
    size_t relsz = 0;

    for (const MochaElfDyn* entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
        switch (entry->d_tag) {
            case DT_SYMTAB: symtab = unity.bias + entry->d_un.d_ptr; break;
            case DT_STRTAB: strtab = unity.bias + entry->d_un.d_ptr; break;
            case DT_STRSZ: strsz = entry->d_un.d_val; break;
            case DT_JMPREL: jmprel = unity.bias + entry->d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = entry->d_un.d_val; break;
            case DT_PLTREL: pltrel = static_cast<long>(entry->d_un.d_val); break;
            case DT_RELA: rela = unity.bias + entry->d_un.d_ptr; break;
            case DT_RELASZ: relasz = entry->d_un.d_val; break;
            case DT_REL: rel = unity.bias + entry->d_un.d_ptr; break;
            case DT_RELSZ: relsz = entry->d_un.d_val; break;
            default: break;
        }
    }

    if (!symtab || !strtab || !strsz) {
        error = "libunity.so dynamic symbol table is incomplete";
        return false;
    }

    Table table;
    table.base = unity.bias;
    table.symbols = reinterpret_cast<const MochaElfSym*>(symtab);
    table.strtab = strtab;
    table.strsz = strsz;

    if (jmprel && pltrelsz) {
        if (pltrel == DT_RELA) {
            patch_rela_table(table, reinterpret_cast<const MochaElfRela*>(jmprel), pltrelsz);
        } else if (pltrel == DT_REL) {
            patch_rel_table(table, reinterpret_cast<const MochaElfRel*>(jmprel), pltrelsz);
        }
    }
    if (!table.patched && rela && relasz) {
        patch_rela_table(table, reinterpret_cast<const MochaElfRela*>(rela), relasz);
    }
    if (!table.patched && rel && relsz) {
        patch_rel_table(table, reinterpret_cast<const MochaElfRel*>(rel), relsz);
    }

    if (!table.patched) {
        error = "no dlsym relocation was found in libunity.so";
        return false;
    }

    DlsymFn original = reinterpret_cast<DlsymFn>(table.original);
    if (!original || original == &mocha_dlsym) {
        original = reinterpret_cast<DlsymFn>(dlsym(RTLD_DEFAULT, "dlsym"));
    }
    if (!original) {
        error = "the real dlsym could not be resolved";
        return false;
    }
    g_real_dlsym.store(original);
    g_installed.store(true);

    log_printf(MOCHA_LOG_INFO, "runtime gate installed on libunity.so (dlsym slot, base=%p)",
               reinterpret_cast<void*>(unity.bias));
    return true;
}

bool runtime_gate_installed() {
    return g_installed.load();
}

void set_runtime_ready_callback(void (*callback)()) {
    g_ready_callback.store(callback);
}

bool wait_for_il2cpp_init(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(g_gate_mutex);
    return g_gate_cv.wait_for(lock, timeout, [] { return g_init_complete; });
}

} // namespace mocha
