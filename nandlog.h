#ifndef __NANDLOG_HEADER_H__
#define __NANDLOG_HEADER_H__

// Header Inclusions ---------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>
#include "nandlog_conf.h"


// Library Version -----------------------------------------------------------------------------------------------------

// The major number changes when anything a consumer built against has to be changed to keep working.
// A consumer that cares can then refuse to build rather than discover it at runtime:
//
//     #if (NANDLOG_VERSION_MAJOR != 1)
//     #error "This application was written for major version 1 of nandlog"
//     #endif
//
// The minor number changes for additions: new functions, new policy knobs, new chip drivers, behaviour that
// does not move a byte on the part and breaks no caller. NANDLOG_VERSION_AT_LEAST() is the test for a feature
// added in a minor release. Note that this versions the LIBRARY; NANDLOG_FORMAT_VERSION below versions the
// format a build writes on the part, and the two move independently
#define NANDLOG_VERSION_MAJOR                       1
#define NANDLOG_VERSION_MINOR                       0

#define NANDLOG_VERSION_AT_LEAST(major, minor)      ((NANDLOG_VERSION_MAJOR > (major)) || \
                                                    ((NANDLOG_VERSION_MAJOR == (major)) && (NANDLOG_VERSION_MINOR >= (minor))))


// On-Flash Page Format Defintions -------------------------------------------------------------------------------------

// Every integer in the structures below is little-endian on the part and on the wire, and the structures are laid
// over native integers, so nandlog.c refuses to build or initialize on a big-endian target

#define NANDLOG_NO_TIMESTAMP                        0xFFFFFFFF

// A page states its own record format in the four bytes a reader looks at first
#define NANDLOG_PAGE_MAGIC                          0x31505454   // 'TTP1': records are opaque to the log
#define NANDLOG_PAGE_MAGIC_FRAMED                   0x32505454   // 'TTP2': each record carries its own length

#define NANDLOG_FRAMING_LENGTH_BYTES                2

#define NANDLOG_PAGE_MAGIC_THIS_BUILD               (NANDLOG_RECORD_FRAMING ? NANDLOG_PAGE_MAGIC_FRAMED : NANDLOG_PAGE_MAGIC)

// The version of the on-flash format, recorded in every metadata slot
#define NANDLOG_FORMAT_VERSION                      1

// The record format: opaque records, or records that carry their own length. A page states it in its magic, and an
// offload stream in its header
#define NANDLOG_RECORD_FORMAT_OPAQUE                1
#define NANDLOG_RECORD_FORMAT_FRAMED                2
#define NANDLOG_RECORD_FORMAT_THIS_BUILD            (NANDLOG_RECORD_FRAMING ? NANDLOG_RECORD_FORMAT_FRAMED : NANDLOG_RECORD_FORMAT_OPAQUE)

// With framing on, a payload is a sequence of records laid out as
//
//     [data length: 2][record type: 1][timestamp: 4][data: data length]
//
// with the length little-endian, so a reader can walk a page without knowing the application's record types

typedef struct __attribute__ ((__packed__))
{
   uint32_t magic;               // NANDLOG_PAGE_MAGIC, or NANDLOG_PAGE_MAGIC_FRAMED
   uint32_t epoch;               // experiment generation; makes stale data from a prior run unmistakable
   uint32_t seq;                 // page index within the epoch, starting at 0
   uint32_t first_timestamp;     // smallest record timestamp in the page, or NANDLOG_NO_TIMESTAMP if it has none
   uint32_t last_timestamp;      // largest record timestamp in the page, since records need not be in time order
   uint16_t payload_length;      // valid payload bytes; zero only in a tombstone
   uint16_t record_count;        // complete records in the payload
   uint32_t payload_crc;         // CRC-32 over payload[0 .. payload_length)
   uint32_t header_crc;          // CRC-32 over this header up to (not including) this field
} nandlog_page_header_t;

// A tombstone replaces a page that could not be read when its block was relocated: a valid header with no payload.
// It keeps the position occupied, so the write head is never found there, and it reads back as a gap
#define NANDLOG_PAGE_IS_TOMBSTONE(header)           ((header)->payload_length == 0)

#define NANDLOG_PAGE_HEADER_CRC_BYTES               (sizeof(nandlog_page_header_t) - sizeof(uint32_t))

// A metadata slot describes one epoch: the header, nandlog_meta_extension_t, the details, then the list of positions
// the epoch has passed over. Each slot is written to both pages of a pair, so one unreadable page does not lose it
// and a boot can tell a finished write from one a power cut interrupted. The digit in the magic is not a version
#define NANDLOG_META_MAGIC                          0x324D5454   // 'TTM2', little-endian

typedef struct __attribute__ ((__packed__))
{
   uint32_t magic;               // NANDLOG_META_MAGIC
   uint32_t epoch;               // the experiment generation this page describes
   uint32_t log_start_page;      // the page the epoch was begun at; always the first page of a block
   uint32_t created_timestamp;   // always 0, since the log reads no clock
   uint16_t details_length;      // bytes of caller-defined metadata in the slot
   uint16_t format_version;      // NANDLOG_FORMAT_VERSION
   uint32_t details_crc;         // CRC-32 over the experiment details blob
   uint32_t header_crc;          // CRC-32 over this header up to (not including) this field
   uint32_t reserved;            // 0xFFFFFFFF; not covered by header_crc
} nandlog_meta_header_t;

typedef struct __attribute__ ((__packed__))
{
   uint32_t sequence;            // one more than the slot written before it, anywhere in the ring
   uint32_t head_failed_page;    // 0xFFFFFFFF, or the latest page at the write head whose program failed or did not
                                 // read back. It may hold charge no read can see, so the log does not program it again
   uint32_t retired_blocks;      // blocks the log knew to be retired when the slot was written
   uint32_t span_retired;        // which of the epoch's first span_blocks blocks were retired: the sum, over each,
                                 // of the CRC-32 of its 32-bit offset from the epoch's first block
   uint16_t span_blocks;         // blocks from the epoch's first through the one holding its newest page, or 0. A
                                 // boot whose bad-block table would lay these out differently seals the epoch
   uint8_t zero;                 // 0
   uint8_t sealed;               // 0xFF, or 0 once the epoch is sealed
   uint16_t passed_over_count;   // runs in the list that follows the details
   uint16_t reserved;            // 0xFFFF
   uint32_t passed_over_crc;     // CRC-32 over that list
   uint32_t extension_crc;       // CRC-32 over the header and this extension up to (not including) this field
} nandlog_meta_extension_t;

// The passed-over list names positions below the write head whose pages a boot found would not read whole, and with
// NANDLOG_PASS_OVER_HEAD_AT_BOOT the head's own page. It is stored as ascending runs, one 32-bit word each: the first
// position in bits 0-23 and the run length less one in bits 24-31. A listed position counts as written whatever its
// page later reads, so a torn page that drifts back to blank is never taken for the write head. An epoch is sealed
// when its list is nearly full

#define NANDLOG_MAX_DATA_BYTES_PER_PAGE             (NANDLOG_MAX_PAGE_SIZE_BYTES - sizeof(nandlog_page_header_t))
#define NANDLOG_META_HEADER_CRC_BYTES               (sizeof(nandlog_meta_header_t) - (2 * sizeof(uint32_t)))
#define NANDLOG_META_EXTENSION_CRC_BYTES            (sizeof(nandlog_meta_header_t) + sizeof(nandlog_meta_extension_t) - sizeof(uint32_t))

#define NANDLOG_MAX_LISTED_EPOCHS                   16

// What the metadata ring still says about one epoch
typedef struct
{
   uint32_t epoch;               // the generation number
   uint32_t created_timestamp;   // always 0, since the log reads no clock
   uint32_t first_page;          // the page it was begun at, which holds sequence 0 unless that block was retired
   uint16_t details_length;      // bytes of caller-defined details the slot carries
   bool is_current;              // whether this is the epoch being written now
   bool is_sealed;               // whether the current epoch is sealed (NANDLOG_ERR_SEALED)
   bool layout_unverified;       // the bad-block table no longer lays the current epoch out as it was written, so
                                 // the epoch is sealed and a read of it may miss pages
} nandlog_epoch_info_t;


// Offload Wire Format -------------------------------------------------------------------------------------------------

#define NANDLOG_STREAM_MAGIC                        0x31535454   // 'TTS1', little-endian

typedef struct __attribute__ ((__packed__))
{
   uint32_t magic;                  // NANDLOG_STREAM_MAGIC
   uint16_t record_format;          // NANDLOG_RECORD_FORMAT_THIS_BUILD of the device: 1 opaque, 2 framed
   uint16_t details_length;         // bytes of caller-defined metadata following this header
   uint32_t total_pages;            // pages that will be sent, including any zero-length gaps
   uint32_t total_payload_bytes;    // sum of all payload lengths
} nandlog_stream_header_t;

#define NANDLOG_MAX_RETRANSMIT_PAGES                256

// A page on the wire takes its record format from the stream header, so begin a new epoch whenever a firmware update
// changes NANDLOG_RECORD_FRAMING
typedef struct __attribute__ ((__packed__))
{
   uint32_t seq;                    // page sequence within the epoch; identifies a gap for retransmission
   uint32_t first_timestamp;        // the smallest record timestamp in the page, or NANDLOG_NO_TIMESTAMP
   uint32_t last_timestamp;         // the largest record timestamp in the page
   uint16_t payload_length;         // 0 means the device could not read this page
   uint16_t record_count;
   uint32_t payload_crc;            // CRC-32 over the payload bytes that follow
} nandlog_wire_page_t;


// Outcomes and Counters -----------------------------------------------------------------------------------------------

// What became of a record or a commit. Anything but NANDLOG_OK from nandlog_store_record() means the record was
// not stored
typedef enum
{
   NANDLOG_OK = 0,                  // stored (or, for a flush, committed or nothing to commit)
   NANDLOG_ERR_NOT_INITIALIZED,     // nandlog_init() has not succeeded
   NANDLOG_ERR_NO_EPOCH,            // no epoch has ever been begun on this part; call nandlog_begin_epoch()
   NANDLOG_ERR_DISABLED,            // nandlog_disable(true), or the part no longer retains pages
   NANDLOG_ERR_EARLIER_EPOCH,       // an earlier epoch is selected for reading
   NANDLOG_ERR_READING,             // a read is open and storing this would have meant committing a page
   NANDLOG_ERR_TOO_LARGE,           // the record can never fit in a page
   NANDLOG_ERR_FULL,                // every page of the log region belongs to the current epoch; begin a new one
   NANDLOG_ERR_UNWRITABLE,          // a commit failed: no block would retain the page, and its records are lost
   NANDLOG_ERR_SEALED               // the epoch takes no more pages until a new one is begun, because its passed-over
                                    // list is full or its slot or layout can no longer be verified
} nandlog_status_t;

// Running totals since nandlog_init(). Records are counted at the API; pages and blocks on the part
typedef struct
{
   uint32_t records_stored;         // nandlog_store_record() calls that returned NANDLOG_OK
   uint32_t records_refused;        // calls that returned anything else; those records were never stored
   uint32_t records_lost;           // records that were stored in RAM but whose page could not be committed
   uint32_t pages_committed;        // pages written and verified by reading them back
   uint32_t pages_relocated;        // pages moved out of a failing block, tombstones included
   uint32_t tombstones_written;     // relocated pages that could not be read, and were replaced by a tombstone
   uint32_t positions_skipped;      // sequence numbers the last boot passed over, which are never programmed again
   uint32_t blocks_retired;         // blocks retired by the log, persistently, since nandlog_init()
   uint32_t reads_at_ecc_limit;     // page reads the part corrected only at the limit of its ECC, a sign of wear
} nandlog_stats_t;


// Public API Functions ------------------------------------------------------------------------------------------------

// Confirm a part is present and answering, without bringing the log up or writing anything to it. Intended
// for builds that only test for its presence; nandlog_init() performs the same check for itself
bool nandlog_probe(void);

// Bring the log up: open the port, identify the part, load its bad-block table, then recover the current
// epoch from the metadata ring and locate the write head within it. Safe to call more than once. False means
// the port could not be opened, or the part did not answer, and no other call should be made
bool nandlog_init(void);

// Release the port. The log can be brought back up with nandlog_init(). Anything still buffered in RAM is
// lost, so flush first
void nandlog_deinit(void);

// Stop or resume accepting records. A disabled log refuses what it is given rather than buffering it, and
// still serves reads and flushes. Nothing is disabled by nandlog_init(). Resuming does not lift the log's own
// refusal once no block will retain a page or the epoch is sealed
void nandlog_disable(bool disable);

// Payload bytes a page can carry on the fitted part: the page size less the header the log writes into it.
// Answered from the chip driver, so it is correct before nandlog_init() has run
uint32_t nandlog_data_bytes_per_page(void);

// Begin a new epoch, described by an opaque caller-defined blob of at most NANDLOG_MAX_EPOCH_DETAILS_BYTES. The
// log neither reads nor interprets the blob. Anything still buffered is committed to the old epoch first, and
// everything logged previously stays on the part, but reads cover only the current epoch unless another is
// selected. Must be called inside a session with no read open, and returns false if it was not, if the blob is
// too large, if the buffered records could not be committed, or if no slot in the metadata ring would take it
bool nandlog_begin_epoch(const void *blob, uint16_t length);

// Copy back the details of whichever epoch is currently selected, truncated to 'length'. Zero-fills if there
// are none, so a caller that never stored any reads zeros rather than stale bytes
void nandlog_retrieve_epoch_details(void *blob, uint16_t length);

// How many epochs the metadata ring still describes, the current one included, capped at
// NANDLOG_MAX_LISTED_EPOCHS. Costs about one header read per epoch listed
uint32_t nandlog_epoch_count(void);

// Describe the index-th epoch, newest first, so index 0 is always the current one. False once the index runs
// past what the ring describes. Costs the same walk as nandlog_epoch_count()
bool nandlog_epoch_info(uint32_t index, nandlog_epoch_info_t *info);

// Point reads at an epoch other than the one being written.
//
// AN EARLIER EPOCH IS NOT PRESERVED, only left alone: the write head sweeps forward from where the previous
// generation stopped and erases each block just before it needs it, so an epoch is eaten from its start as
// the current one grows. What comes back is whatever is left, and a page that has been swept over reads as a
// gap exactly like one that rotted. The read ends at the last page the epoch wrote. A read that yields nothing
// means the epoch is gone, not that it never existed -- compare against nandlog_epoch_info(), which still
// describes it from the ring.
//
// Writing is refused while an earlier epoch is selected, so a download cannot be confused for a place to put
// data. Must be called inside a session, fails while a read is open, and returns false if no slot in the ring
// names that epoch. nandlog_select_current_epoch() goes back, and so does ending the session
bool nandlog_select_epoch(uint32_t epoch);
void nandlog_select_current_epoch(void);

// Append one record to the page being assembled in RAM. With NANDLOG_RECORD_FRAMING on, the record is
// prefixed with its own data length. 'record_type' and 'data' are opaque to the log; the timestamp is only
// ever compared, never interpreted, so its epoch and units are the caller's business. A record stamped
// NANDLOG_NO_TIMESTAMP keeps that stamp, but its page's time bounds count it as 0xFFFFFFFE. Committing a full page
// happens here, so this call occasionally writes to flash. Records are never split across pages. A read already
// open does not serve records stored after it opened.
//
// Returns NANDLOG_OK if the record was stored, and otherwise why it was not. A commit that fails here loses the
// records already buffered too, and counts them in records_lost
nandlog_status_t nandlog_store_record(uint8_t record_type, uint32_t timestamp, const void *data, uint32_t data_length);

// Commit what is buffered. A page is written automatically as soon as the next record will not fit, so this
// is only needed to force out a partial page. Pass false to write only if a whole page has accumulated. On an
// enabled log, a call that commits nothing erases the next block ahead of the write head if one is due, so an
// idle nandlog_flush(false) is a good place for that work. Returns the outcome of the commit, NANDLOG_OK if there
// was nothing to commit, or why it was refused
nandlog_status_t nandlog_flush(bool write_partial_pages);

// Whether any records are sitting unwritten in RAM
bool nandlog_has_buffered_data(void);

// Hold the part powered across a run of operations, instead of waking and sleeping it around each one.
// Reading and storing metadata both require a session; ordinary logging does not. Sessions do not nest
void nandlog_begin_session(void);
void nandlog_end_session(void);

// Open a read over the selected epoch, bounded by timestamp. Zero for either bound means "from the beginning"
// and "to the end"; both bounds are resolved here, so nothing downstream has to seek again. Resolving a bound reads
// every page header in the epoch with the library's lock held. Every page with a record inside the bounds is
// selected, and possibly some pages without one. Buffered records are committed first. Has no effect outside a
// session. While a read is open, writing a page is refused, so to keep logging through a long download, note the
// read's first sequence number and page count, close it, and fetch the pages with nandlog_retrieve_page_by_seq()
void nandlog_begin_reading(uint32_t starting_timestamp, uint32_t ending_timestamp);

// Close a read, whether or not it ran to completion. Implied by nandlog_end_session()
void nandlog_end_reading(void);

// Measure the open read: how many pages it will yield, including any that turn out to be unreadable, and how
// many payload bytes they hold. Either pointer may be NULL. The page count is arithmetic, but an exact byte
// total costs a read of every page in the span -- doubling the flash traffic of a download -- so pass NULL
// for 'num_bytes' unless the figure is needed exactly. Reports zero for both if no read is open
void nandlog_read_span(uint32_t *num_pages, uint32_t *num_bytes);

// Take the next page of the open read, returning its payload length. 'buffer' needs room for
// NANDLOG_MAX_DATA_BYTES_PER_PAGE bytes. Pass NULL for 'header' to ignore the framing metadata. Zero means that
// page was unreadable or a tombstone, which is not the end of the read; a page whose header could not be read
// comes back with 'seq' set to its position, so the gap is still identifiable. The read closes after its last page
uint32_t nandlog_retrieve_next_page(uint8_t *buffer, nandlog_page_header_t *header);

// Fetch one page of the selected epoch by its sequence number, so a host can re-request what it lost. No read
// need be open. Returns zero, with the header describing the gap, if that page cannot be produced. 'buffer' is
// sized as for nandlog_retrieve_next_page()
uint32_t nandlog_retrieve_page_by_seq(uint32_t seq, uint8_t *buffer, nandlog_page_header_t *header);

// Step through the records of a payload belonging to a page that announced NANDLOG_PAGE_MAGIC_FRAMED.
bool nandlog_framed_next_record(const uint8_t *payload, uint32_t length, uint32_t *offset, const uint8_t **record, uint32_t *record_bytes);

// The CRC-32 the log writes and checks, continued from 'crc' as Python's zlib.crc32(data, crc) does, so a served
// page's payload_crc is nandlog_crc32(0, buffer, length). It calls nandlog_port_crc32() if NANDLOG_PORT_CRC32 is
// set, and otherwise needs neither nandlog_init() nor the library's lock
uint32_t nandlog_crc32(uint32_t crc, const void *data, uint32_t length);

// Walk backwards from the write head, newest page first, independently of any open read. Zero means that
// page could not be read, which is not the end of the log; 'end_of_epoch' is the signal to stop. 'buffer' is
// sized as for nandlog_retrieve_next_page(), and may be NULL
uint32_t nandlog_read_recent_page(uint32_t pages_back, uint8_t *buffer, nandlog_page_header_t *header, bool *end_of_epoch);

// Accumulate the sequence numbers a host reports missing, across as many calls as it takes, then serve them
// back. nandlog_retransmit_add() returns the number held; anything beyond NANDLOG_MAX_RETRANSMIT_PAGES is
// dropped. An index past the end yields a zero-length page rather than nothing, so the host can tell
// "still missing" from "never answered". Like nandlog_retrieve_page_by_seq(), these need no open read
void nandlog_retransmit_clear(void);
uint32_t nandlog_retransmit_add(const uint32_t *seqs, uint32_t count);
uint32_t nandlog_retransmit_count(void);
uint32_t nandlog_retransmit_total_bytes(void);
uint32_t nandlog_retrieve_retransmit_page(uint32_t index, uint8_t *buffer, nandlog_page_header_t *header);

// Retired blocks, factory-marked and grown, across the metadata ring and log region. Answers from RAM
uint32_t nandlog_bad_block_count(void);

// Copy out the running totals since nandlog_init()
void nandlog_get_stats(nandlog_stats_t *stats);

// RECOVERY UTILITY. Discard the persisted bad-block table so it is rebuilt on the next boot. What this can
// achieve depends on the part: one that remaps in hardware cannot forget. The table in RAM stands until the
// reboot, and a rebuilt table that lays the current epoch out differently seals it
void nandlog_reset_bad_block_table(void);

#endif  // #ifndef __NANDLOG_HEADER_H__
