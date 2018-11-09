#include "clint.h"
#include "i-virtual-state-access.h"
#include "machine.h"
#include "pma.h"
#include "rtc.h"
#include "riscv-constants.h"

#define CLINT_MSIP0    0
#define CLINT_MTIMECMP 0x4000
#define CLINT_MTIME    0xbff8

static bool clint_read_msip(i_virtual_state_access *a, uint64_t *val,
    int size_log2) {
    if (size_log2 == 2) {
        *val = ((a->read_mip() & MIP_MSIP) == MIP_MSIP);
        return true;
    }
    return false;
}

static bool clint_read_mtime(i_virtual_state_access *a, uint64_t *val, int size_log2) {
    if (size_log2 == 3) {
        *val = rtc_cycle_to_time(a->read_mcycle());
        return true;
    }
    return false;
}

static bool clint_read_mtimecmp(i_virtual_state_access *a, uint64_t *val, int size_log2) {
    if (size_log2 == 3) {
        *val = a->read_mtimecmp();
        return true;
    }
    return false;
}

/// \brief CLINT device read callback. See ::pma_read.
static bool clint_read(const pma_entry *pma, i_virtual_state_access *a, uint64_t offset, uint64_t *val, int size_log2) {
    (void) pma;

    switch (offset) {
        case CLINT_MSIP0:    // Machine software interrupt for hart 0
            return clint_read_msip(a, val, size_log2);
        case CLINT_MTIMECMP: // mtimecmp
            return clint_read_mtimecmp(a, val, size_log2);
        case CLINT_MTIME:    // mtime
            return clint_read_mtime(a, val, size_log2);
        default:
            // other reads are exceptions
            return false;
    }
}

/// \brief CLINT device read callback. See ::pma_write.
static bool clint_write(const pma_entry *pma, i_virtual_state_access *a, uint64_t offset, uint64_t val, int size_log2) {
    (void) pma;

    switch (offset) {
        case CLINT_MSIP0: // Machine software interrupt for hart 0
            if (size_log2 == 2) {
                //??D I don't yet know why Linux tries to raise MSIP when we only have a single hart
                //    It does so repeatedly before and after every command run in the shell
                //    Will investigate.
                if (val & 1) {
                    a->set_mip(MIP_MSIP);
                } else {
                    a->reset_mip(MIP_MSIP);
                }
                return true;
            }
            return false;
        case CLINT_MTIMECMP: // mtimecmp
            if (size_log2 == 3) {
                a->write_mtimecmp(val);
                a->reset_mip(MIP_MTIP);
                return true;
            }
            // partial mtimecmp is not supported
            return false;
        default:
            // other writes are exceptions
            return false;
    }
}

#define base(v) ((v) - ((v) % (PMA_PAGE_SIZE)))
#define offset(v) ((v) % (PMA_PAGE_SIZE))
/// \brief CLINT device peek callback. See ::pma_peek.
static bool clint_peek(const pma_entry *pma, uint64_t page_index, const uint8_t **page_data, uint8_t *scratch) {
    const machine_state *s = reinterpret_cast<const machine_state *>(pma_get_context(pma));
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
        "code assumes little-endian byte ordering");
    // There are 3 non-pristine pages: base(CLINT_MSIP0), base(CLINT_MTIMECMP), and base(CLINT_MTIME)
    switch (page_index) {
        case base(CLINT_MSIP0):
            // This page contains only msip (which is either 0 or 1)
            // Since we are little-endian, we can simply write the bytes
            memset(scratch, 0, PMA_PAGE_SIZE);
            *reinterpret_cast<uint64_t *>(scratch + offset(CLINT_MSIP0)) = ((machine_read_mip(s) & MIP_MSIP) == MIP_MSIP);
            *page_data = scratch;
            return true;
        case base(CLINT_MTIMECMP):
            memset(scratch, 0, PMA_PAGE_SIZE);
            *reinterpret_cast<uint64_t *>(scratch + offset(CLINT_MTIMECMP)) = machine_read_mtimecmp(s);
            *page_data = scratch;
            return true;
        case base(CLINT_MTIME):
            memset(scratch, 0, PMA_PAGE_SIZE);
            *reinterpret_cast<uint64_t*>(scratch + offset(CLINT_MTIME)) = rtc_cycle_to_time(machine_read_mcycle(s));
            *page_data = scratch;
            return true;
        default:
            *page_data = nullptr;
            if (page_index % PMA_PAGE_SIZE == 0) return true;
            else return false;
    }
}
#undef base
#undef offset

static const pma_driver clint_driver = {
    "CLINT",
    clint_read,
    clint_write,
    clint_peek
};

bool clint_register_mmio(machine_state *s, uint64_t start, uint64_t length) {
    return machine_register_mmio(s, start, length, s, &clint_driver);
}
