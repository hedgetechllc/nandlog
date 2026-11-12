#ifndef __NANDLOG_CONF_HEADER_H__
#define __NANDLOG_CONF_HEADER_H__

// Hardware Presence ---------------------------------------------------------------------------------------------------

// Whether a NAND part is fitted in this build. When it is not, the library compiles to stand-ins that accept
// everything and return nothing, so the application above it needs no conditionals of its own
#ifndef NANDLOG_HAS_HARDWARE
#define NANDLOG_HAS_HARDWARE                        1
#endif


// Static Sizing Budget ------------------------------------------------------------------------------------------------

// The largest page and spare area the host is prepared to set aside RAM for. Buffers throughout the library
// and the application above it are sized from these which is what keeps every allocation static. A part
// whose page exceeds the budget is a compile error inside that part's driver, so the only cost of setting
// these larger than the fitted part needs is unused RAM
#define NANDLOG_MAX_PAGE_SIZE_BYTES                 4096
#define NANDLOG_MAX_SPARE_SIZE_BYTES                256


// Log Policy ----------------------------------------------------------------------------------------------------------

// Erase attempts on one block before it is retired as bad. A program is never repeated, because a page that reported
// a failure may hold a partial program, so a single failed program retires its block
#define NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL         3

// Blocks kept erased ahead of the write head, so a page write never waits on an erase. Every boot erases them again,
// because an interrupted erase can leave a block that reads erased but programs unreliably, so a device that reboots
// far more often than it fills a block should set this to 0 to save that wear
#ifndef NANDLOG_ERASE_AHEAD_BLOCKS
#define NANDLOG_ERASE_AHEAD_BLOCKS                  2
#endif

// Blocks a single page write may try, its own included, before the log concludes the part will not hold it. Each
// further block costs an erase plus a block's worth of page copies, so this is a bound on real time as much as on
// blocks: it has to be large enough to ride out a genuine cluster of worn blocks and small enough that a part
// which has stopped programming altogether is diagnosed in seconds rather than starving the system
#define NANDLOG_PAGE_PLACEMENT_ATTEMPTS             3

// Largest caller-defined metadata blob stored alongside the log, at most the page size less 320 bytes. It shares a
// page with the epoch's list of passed-over positions, so every byte it takes shortens that list. An epoch whose
// details are longer than this build allows is sealed until a new epoch is begun
#define NANDLOG_MAX_EPOCH_DETAILS_BYTES             512

// A record whose timestamp steps backwards by no more than this is treated as writer disagreement and kept as
// stamped in the same page, rather than as the time base having moved and the page having to be committed. It is in
// the units of the application's timestamps, which the log never interprets
#define NANDLOG_TIMESTAMP_TOLERANCE_MS              250

// Whether a boot passes over the page at the write head, rather than program it, when the head is part-way into a
// block, because a power cut can leave that page to be programmed a second time. Each such boot after new records
// takes a run of the epoch's passed-over list, which holds about 876 on a 4 KB page before the epoch is sealed, so
// this suits a device that rarely reboots and may lose power mid-program. See the getting-started guide
#ifndef NANDLOG_PASS_OVER_HEAD_AT_BOOT
#define NANDLOG_PASS_OVER_HEAD_AT_BOOT              0
#endif

// A value that differs from one boot to the next, such as a reset-surviving boot counter or a random number, if the
// host has one. The write-head search rotates its first probes by it to spread read disturb. Any value is safe
#ifndef NANDLOG_BOOT_SALT
#define NANDLOG_BOOT_SALT                           0
#endif

// Whether each record carries its own length so that a reader can walk a page without knowing the
// application's record types
#ifndef NANDLOG_RECORD_FRAMING
#define NANDLOG_RECORD_FRAMING                      1
#endif

// How long to wait for the chip to clear BUSY after a program or erase before declaring it dead, as a number of
// status polls NANDLOG_BUSY_POLL_INTERVAL_US apart. The wait is at least 0.5 s here, plus the time the reads take
#define NANDLOG_BUSY_POLL_INTERVAL_US               10
#define NANDLOG_BUSY_TIMEOUT_POLLS                  50000

// Whether a page being relocated may be moved inside the chip instead of across the bus. Each driver already
// answers for its own part, so this exists only to take the path out of a build whose part turns out to
// mishandle it
#ifndef NANDLOG_CHIP_PAGE_COPY
#define NANDLOG_CHIP_PAGE_COPY                      1
#endif


// CRC-32 --------------------------------------------------------------------------------------------------------------

// Whether the port computes the log's CRC-32 in nandlog_port_crc32(), on a hardware engine for example, rather than
// the library in software. nandlog_port.h gives the contract
#ifndef NANDLOG_PORT_CRC32
#define NANDLOG_PORT_CRC32                          0
#endif

// Whether the software CRC-32's lookup table is kept in RAM rather than in flash, which is faster where flash reads
// have wait states. It costs 1 KiB of RAM, or 4 KiB with slicing-by-4
#ifndef NANDLOG_CRC_TABLE_IN_RAM
#define NANDLOG_CRC_TABLE_IN_RAM                    1
#endif

// Whether the software CRC-32 takes four bytes a step through four tables (slicing-by-4) rather than one byte at a
// time. It is about twice as fast for 3 KiB more table, and of little use when nandlog_port_crc32() uses hardware
#ifndef NANDLOG_CRC_SLICE_BY_4
#define NANDLOG_CRC_SLICE_BY_4                      0
#endif


// Diagnostics ---------------------------------------------------------------------------------------------------------

// Which of the library's messages reach nandlog_port_log(): 0 none, 1 errors, 2 errors and warnings, 3 all of them.
// Messages left out are compiled out, and the library behaves the same at every level
#ifndef NANDLOG_LOG_LEVEL
#define NANDLOG_LOG_LEVEL                           3
#endif

#endif  // #ifndef __NANDLOG_CONF_HEADER_H__
