/* PANDABEGINCOMMENT
 * 
 * Authors:
 * Luke Craig luke.craig@ll.mit.eduß
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include <linux/auxvec.h>
#include <linux/elf.h>
#include <string>
#include "panda/plugin.h"
#include "panda/plugin_plugin.h"

using namespace std;

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
#include "syscalls2/syscalls_ext_typedefs.h"
#include "syscalls2/syscalls2_info.h"
#include "syscalls2/syscalls2_ext.h"
#include "proc_start_linux.h"
#include "proc_start_linux_ppp.h"
PPP_PROT_REG_CB(on_rec_auxv);
PPP_CB_BOILERPLATE(on_rec_auxv);
}

// uncomment to look under the hood
//#define DEBUG

#ifdef DEBUG
#define log(...) printf(__VA_ARGS__)
#else
#define log(...)
#endif


#if defined(TARGET_WORDS_BIGENDIAN)
#if TARGET_LONG_SIZE == 4
#define fixupendian(x)         {x=bswap32((target_ptr_t)x);}
#else
#define fixupendian(x)         {x=bswap64((uint64_t)x);}
#endif
#else
#define fixupendian(x) {}
#endif


#if TARGET_LONG_BITS == 32
#define ELF(r) Elf32_ ## r
#else
#define ELF(r) Elf64_ ## r
#endif

void *self_ptr;
panda_cb pcb_sbe_execve;

string read_str(CPUState* cpu, target_ulong ptr){
    string buf = "";
    char tmp;
    while (true){
        if (panda_virtual_memory_read(cpu, ptr, (uint8_t*)&tmp,1) == MEMTX_OK){
            buf += tmp;
            if (tmp == '\x00'){
                break;
            }
            ptr+=1;
        }else{
            break;
        }
    }
    return buf;
}

#define FAIL_READ_ARGV -1
#define FAIL_READ_ENVP -2
#define FAIL_READ_AUXV -3


/**
 * 
 * The stack layout in the first block of a linux process looks like this:
 * 
 * |‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|
 * |             Auxiliary vector           |
 * |________________________________________|
 * |                                        |
 * |                  environ               |
 * |________________________________________|
 * |‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|
 * |                   argv                 |
 * |________________________________________|
 * |‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|
 * |                   Stack                |
 * |________________________________________|
 * 
 * The Stack grows down.
 * 
 */ 

int read_aux_vals(CPUState *cpu, struct auxv_values *vals){
    target_ulong sp = panda_current_sp(cpu);
    
    // keep track of where on the stack we are
    int ptrlistpos = 1;
    target_ulong ptr;

    /**
     * Read the argv values to the program.
     */
    vals->argv_ptr_ptr = sp + (ptrlistpos * sizeof(target_ulong));
    int argc_num = 0;
    while (true){
        if (panda_virtual_memory_read(cpu, sp + (ptrlistpos * sizeof(target_ulong)), (uint8_t *)&ptr, sizeof(ptr)) != MEMTX_OK){
            return FAIL_READ_ARGV;
        }
        ptrlistpos++;
        if (ptr == 0){
            break;
        } else if (argc_num < MAX_NUM_ARGS){
            string arg = read_str(cpu, ptr);
            if (arg.length() > 0)
            {
                strncpy(vals->argv[argc_num], arg.c_str(), MAX_PATH_LEN);
                vals->arg_ptr[argc_num] = ptr;
                argc_num++;
            }
        }
    }
    vals->argc = argc_num;

    /**
     * Read the environ values from the stack
     */ 
    vals->env_ptr_ptr = sp + (ptrlistpos * sizeof(target_ulong));
    int envc_num = 0;
    while (true){
        if (panda_virtual_memory_read(cpu, sp + (ptrlistpos * sizeof(target_ulong)), (uint8_t *)&ptr, sizeof(ptr)) != MEMTX_OK){
            return FAIL_READ_ENVP;
        }
        ptrlistpos++;
        if (ptr == 0){
            break;
        } else if (envc_num < MAX_NUM_ENV){
            string arg = read_str(cpu, ptr);
            if (arg.length() > 0){
                strncpy(vals->envp[envc_num], arg.c_str(), MAX_PATH_LEN);
                vals->env_ptr[envc_num] = ptr;
                envc_num++;
            }
        }
    }
    vals->envc = envc_num;

    /**
     * Read the auxiliary vector
     */ 
    target_ulong entrynum, entryval;
    while (true){
        if (panda_virtual_memory_read(cpu, sp + (ptrlistpos * sizeof(target_ulong)), (uint8_t *)&entrynum, sizeof(entrynum)) != MEMTX_OK || panda_virtual_memory_read(cpu, sp + ((ptrlistpos + 1) * sizeof(target_ulong)), (uint8_t *)&entryval, sizeof(entryval))){
            return FAIL_READ_AUXV;
        }
        ptrlistpos += 2;
        fixupendian(entrynum);
        fixupendian(entryval);
        if (entrynum == AT_NULL){
            break;
        }else if (entrynum == AT_ENTRY){
            vals->entry = entryval;
        }else if (entrynum == AT_PHDR){
            vals->phdr = entryval;
            // every elf I've seen says that the PHDR
            // is immediately following the EHDR.
            // we can do a bunch to check this or we can just
            // take the value.
            vals->program_header = entryval - sizeof(ELF(Ehdr));
        }else if (entrynum == AT_EXECFN){
            vals->execfn_ptr = entryval;
            string execfn = read_str(cpu, entryval);
            execfn.copy(vals->execfn, MAX_PATH_LEN - 1, 0);
        }else if (entrynum == AT_SYSINFO_EHDR){
            vals->ehdr = entryval;
        }else if (entrynum == AT_HWCAP){
            vals->hwcap = entryval;
        }else if (entrynum == AT_HWCAP2){
            vals->hwcap2 = entryval;
        }else if (entrynum == AT_PAGESZ){
            vals->pagesz = entryval;
        }else if (entrynum == AT_CLKTCK){
            vals->clktck = entryval;
        }else if (entrynum == AT_PHENT){
            vals->phent = entryval;
        }else if (entrynum == AT_PHNUM){
            vals->phnum = entryval;
        }else if (entrynum == AT_BASE){
            vals->base = entryval;
        }else if (entrynum == AT_FLAGS){
            vals->flags = entryval;
        }else if (entrynum == AT_UID){
            vals->uid = entryval;
        }else if (entrynum == AT_EUID){
            vals->euid = entryval;
        }else if (entrynum == AT_GID){
            vals->gid = entryval;
        }else if (entrynum == AT_EGID){
            vals->egid = entryval;
        }else if (entrynum == AT_SECURE){
            vals->secure = entryval;
        }else if (entrynum == AT_RANDOM){
            vals->random = entryval;
        }else if (entrynum == AT_PLATFORM){
            vals->platform = entryval;
        }
    }
    return 0;
}

bool has_been_in_kernel = false;

void sbe(CPUState *cpu, TranslationBlock *tb){
    bool in_kernel = panda_in_kernel_code_linux(cpu);
    if (unlikely(!panda_in_kernel_code_linux(cpu) && has_been_in_kernel)){
        // check that we can read the stack
        target_ulong sp = panda_current_sp(cpu);
        target_ulong argc;
        if (panda_virtual_memory_read(cpu, sp, (uint8_t *)&argc, sizeof(argc)) == MEMTX_OK){
            struct auxv_values *vals = (struct auxv_values*)malloc(sizeof(struct auxv_values));
            memset(vals, 0, sizeof(struct auxv_values));
            int status = read_aux_vals(cpu, vals);
            if (!status && vals->entry && vals->phdr){
                PPP_RUN_CB(on_rec_auxv, cpu, tb, vals);
            }else if (status == FAIL_READ_ARGV){
                log("failed to read argv\n");
            }else if (status == FAIL_READ_ENVP){
                log("failed to read envp\n");
            }else if (status == FAIL_READ_AUXV){
                log("failed to read auxv\n");
            }
            panda_disable_callback(self_ptr, PANDA_CB_START_BLOCK_EXEC, pcb_sbe_execve);
            free(vals);
            has_been_in_kernel = false;
        }else{
            // this would be the case where fault_hooks would work well.
            // printf("got here and could not read stack\n");
        }
    } else if (in_kernel){
        has_been_in_kernel = true;
    }
}

void execve_cb(CPUState *cpu, target_ptr_t pc, target_ptr_t filename, target_ptr_t argv, target_ptr_t envp) {
    panda_enable_callback(self_ptr, PANDA_CB_START_BLOCK_EXEC, pcb_sbe_execve);
}

void execveat_cb (CPUState* cpu, target_ptr_t pc, int dfd, target_ptr_t filename, target_ptr_t argv, target_ptr_t envp, int flags) {
    panda_enable_callback(self_ptr, PANDA_CB_START_BLOCK_EXEC, pcb_sbe_execve);
}

bool init_plugin(void *self) {
    self_ptr = self;

    #if defined(TARGET_MIPS64)
        fprintf(stderr, "[ERROR] proc_start_linux: mips64 architecture not supported!\n");
        return false;
    #elif defined(TARGET_PPC)
        fprintf(stderr, "[ERROR] proc_start_linux: PPC architecture not supported by syscalls2!\n");
        return false;
    #else
        pcb_sbe_execve.start_block_exec = sbe;
        panda_register_callback(self, PANDA_CB_START_BLOCK_EXEC, pcb_sbe_execve);
        panda_disable_callback(self, PANDA_CB_START_BLOCK_EXEC, pcb_sbe_execve);

        // why? so we don't get 1000 messages telling us syscalls2 is already loaded
        void* syscalls2 = panda_get_plugin_by_name("syscalls2");
        if (syscalls2 == NULL){
            panda_require("syscalls2");
        }
        assert(init_syscalls2_api());
        PPP_REG_CB("syscalls2", on_sys_execve_enter, execve_cb);
        PPP_REG_CB("syscalls2", on_sys_execveat_enter, execveat_cb);
    #endif
    return true;
}

void uninit_plugin(void *self) {
#if defined(TARGET_PPC) or defined(TARGET_MIPS64)
#else
  void* syscalls = panda_get_plugin_by_name("syscalls2");
  if (syscalls != NULL){
    PPP_REMOVE_CB("syscalls2", on_sys_execve_enter, execve_cb);
    PPP_REMOVE_CB("syscalls2", on_sys_execveat_enter, execveat_cb);
  }
#endif
}
