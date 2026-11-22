/* RTC memory survives a software reset and MCUboot's DRAM initialization.
 * Capture only the faulting Xtensa state and its bounded stack window. */
#include <nuttx/config.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <elf.h>
#include <nuttx/board.h>
#include <nuttx/elf.h>
#include <nuttx/sched.h>
#include "esp32s3_reset_reasons.h"
#include "fc_port.h"

struct core_image {
    Elf32_Ehdr elf;
    Elf32_Phdr segment[2];
    Elf32_Nhdr note;
    char owner[8];
    elf_prstatus_t status;
    uint8_t stack[1024];
};

static struct {
    uint32_t magic, pending, checksum;
    size_t length;
    struct core_image core;
} record __attribute__((section(".rtc.bss"), aligned(4)));

#define DUMP_MAGIC 0x46434436u

static uint32_t checksum(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t hash = 2166136261u;
    while (n--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

void board_crashdump(uintptr_t sp, struct tcb_s *tcb, const char *file, int line, const char *msg, void *regs)
{
    record.magic = 0;
    if (!regs || !tcb) return;
    struct core_image *c = &record.core;
    memset(c, 0, sizeof(*c));
    c->elf = (Elf32_Ehdr){
        .e_ident = {0x7f, 'E', 'L', 'F', ELFCLASS32, ELFDATA2LSB, EV_CURRENT},
        .e_type = ET_CORE, .e_machine = EM_XTENSA, .e_version = EV_CURRENT,
        .e_ehsize = sizeof(c->elf), .e_phoff = offsetof(struct core_image, segment),
        .e_phentsize = sizeof(c->segment[0]), .e_phnum = 1,
    };
    c->segment[0] = (Elf32_Phdr){.p_type = PT_NOTE, .p_offset = offsetof(struct core_image, note),
        .p_filesz = offsetof(struct core_image, stack) - offsetof(struct core_image, note), .p_align = 4};
    memcpy(c->owner, "CORE", 5);
    c->note = (Elf32_Nhdr){.n_namesz = 5, .n_descsz = sizeof(c->status), .n_type = NT_PRSTATUS};
    c->status.pr_pid = tcb->pid;
    const uint32_t *r = regs;
    xtensa_gregset_t g = {.pc = r[REG_PC], .ps = r[REG_PS], .sar = r[REG_SAR],
        .lbeg = r[REG_LBEG], .lend = r[REG_LEND], .lcount = r[REG_LCOUNT], .windowstart = 1};
    memcpy(g.ar, r + REG_A0, 16 * sizeof(uint32_t));
    memcpy(c->status.pr_regs, &g, sizeof(g));
    record.length = offsetof(struct core_image, stack);
    uintptr_t base = (uintptr_t)tcb->stack_base_ptr;
    uintptr_t top = base + tcb->adj_stack_size;
    sp = r[REG_A1];
    if (sp >= base && sp < top) {
        size_t len = top - sp < sizeof(c->stack) ? top - sp : sizeof(c->stack);
        memcpy(c->stack, (const void *)sp, len);
        c->segment[1] = (Elf32_Phdr){.p_type = PT_LOAD, .p_offset = record.length,
            .p_vaddr = sp, .p_filesz = len, .p_memsz = len, .p_flags = PF_R | PF_W, .p_align = 1};
        c->elf.e_phnum++;
        record.length += len;
    }
    record.checksum = checksum(c, record.length);
    record.pending = 1;
    __atomic_store_n(&record.magic, DUMP_MAGIC, __ATOMIC_RELEASE);
}

size_t fc_port_coredump_size(void)
{
    if (record.magic != DUMP_MAGIC || record.length < offsetof(struct core_image, stack) ||
        record.length > sizeof(record.core) || checksum(&record.core, record.length) != record.checksum) return 0;
    return record.length;
}

int fc_port_coredump_read(size_t off, void *buf, size_t len)
{
    size_t size = fc_port_coredump_size();
    if (!size || off > size || len > size - off) return -1;
    memcpy(buf, (const uint8_t *)&record.core + off, len);
    return 0;
}

void fc_port_coredump_erase(void)
{
    memset(&record, 0, sizeof(record));
}

static int crashed;
void fc_nuttx_boot_reason(void)
{
    soc_reset_reason_t reason = esp32s3_reset_reasons(0);
    crashed = reason == RESET_REASON_CORE_MWDT0 || reason == RESET_REASON_CORE_MWDT1 ||
        reason == RESET_REASON_CORE_RTC_WDT || reason == RESET_REASON_CPU0_MWDT0 ||
        reason == RESET_REASON_CPU0_MWDT1 || reason == RESET_REASON_CPU0_RTC_WDT ||
        reason == RESET_REASON_SYS_RTC_WDT || reason == RESET_REASON_SYS_SUPER_WDT ||
        (record.pending == 1 && fc_port_coredump_size() != 0);
    record.pending = 0;
}

int fc_nuttx_restart_was_crash(void) { return crashed; }
