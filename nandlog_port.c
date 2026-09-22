// TEMPLATE. THIS IS THE ONE FILE AN INTEGRATOR WRITES.
//
// Copy it into your own tree rather than editing it in place, fill in the stubs, and add it to your build
// in place of this one. Every function below states what the layer above assumes of it; those assumptions
// are the contract, and a port that does not hold them up will fail in ways the log cannot diagnose.
//
// A worked implementation for an Ambiq Apollo4 under FreeRTOS is in the README.

#error "nandlog_port.c is a template: implement the stubs below for your platform, then delete this line"


// Header Inclusions ---------------------------------------------------------------------------------------------------

#include "nandlog_port.h"
// ... and whatever your platform needs: its HAL, its RTOS, its board header


// Initialization ------------------------------------------------------------------------------------------------------

bool nandlog_port_init(void)
{
   // Bring up everything needed to talk to the part and leave it ready for a transfer:
   //
   //   - configure the SPI peripheral (mode 0, MSB first, at whatever clock the part is rated for)
   //   - configure SCK, MISO, MOSI and CS
   //   - configure the write-protect pin as an output, and the HOLD pin if the part has one, and drive
   //     both to their inactive state
   //
   // Called by nandlog_probe() and nandlog_init(), and again after nandlog_port_deinit(). False means the
   // log does not come up at all, and no other library call will be made
   return false;
}

void nandlog_port_deinit(void)
{
   // Release the peripheral. The log may be brought back up afterwards with nandlog_init(), so this has to
   // leave things in a state nandlog_port_init() can start from again
}


// Serialization -------------------------------------------------------------------------------------------------------

void nandlog_port_lock(void)
{
   // Serialize the library against itself. Every public function takes this for its whole duration and does
   // its work in a _locked internal, so a caller never holds it and these never nest.
   //
   // A single-threaded system can leave both of these empty. Anything else must supply a real mutex. The log
   // has one page cache and one transfer buffer, and reading a page is a SEQUENCE of commands against the one
   // cache register inside the part -- latch the page, then clock it out. Two callers interleaving those
   // sequences read each other's pages, and nothing about the bus traffic looks wrong.
   //
   // If the lock cannot be taken in every context this is called from -- before the scheduler starts, or from
   // an interrupt -- detect that case and skip the lock rather than blocking or failing
}

void nandlog_port_unlock(void)
{
   // Release what nandlog_port_lock() took under exactly the same condition it was taken under
}


// SPI Transfers -------------------------------------------------------------------------------------------------------

void nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length)
{
   // Send one command byte followed by 'address_length' address bytes, then clock 'read_length' bytes back
   // into 'read_buffer', with chip-select asserted across the whole exchange.
   //
   //   - 'address' may be NULL with 'address_length' zero
   //   - 'read_length' may be zero, and that transfer must still be issued: a part can require the empty
   //     read to complete the command
   //   - 'read_length' may exceed a whole page, so split it across as many transactions as the peripheral's
   //     maximum requires, keeping chip-select asserted between them
   //
   // A transfer either completes or does not return. There is no error path here, so a failure that cannot
   // be retried has to end in nandlog_port_fatal()
}

void nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length)
{
   // The same exchange in the other direction: one command byte, 'address_length' address bytes, then
   // 'write_length' bytes out of 'write_buffer', chip-select asserted throughout, split across transactions
   // if the peripheral has a maximum. The same rules about zero lengths, NULL addresses and failure apply
}


// Write Protection ----------------------------------------------------------------------------------------------------

void nandlog_port_write_enable(bool enable)
{
   // Gate the part's write-protect pin: true permits programming and erasing.
   //
   // Which electrical level that is depends on the board, not on the part -- most WP# pins are active low,
   // but a board may invert one. Get this backwards and every program silently fails
}


// Power ---------------------------------------------------------------------------------------------------------------

void nandlog_port_power(bool awake)
{
   // Wake the SPI peripheral, or return it to its lowest-power state. The log wakes the part around each
   // operation and sleeps it again afterwards, except inside a maintenance session, where it stays awake.
   //
   // This is about the peripheral on the host side; putting the part itself to sleep is the chip driver's
   // nandlog_chip_low_power(). A system with nothing to gain can leave this empty
}


// Diagnostics ---------------------------------------------------------------------------------------------------------

void nandlog_port_log(const char *format, ...)
{
   // Route the library's diagnostics wherever yours go: a console, a ring buffer, nowhere at all. printf
   // style, and called rarely -- retired blocks, an unwritable part, a refused metadata write.
   //
   // Note that this can be reached from inside a page commit, so it must not call back into nandlog
}


// Delays --------------------------------------------------------------------------------------------------------------

void nandlog_port_delay_us(uint32_t microseconds)
{
   // Busy-wait. Used to pace the poll of the part's BUSY bit, so it is called often and must be cheap and
   // must not yield to a scheduler
}

void nandlog_port_delay_ms(uint32_t milliseconds)
{
   // Coarse delay, used only while waiting for a part to come up at probe time. Yielding here is fine
}


// Failure -------------------------------------------------------------------------------------------------------------

void nandlog_port_fatal(const char *reason)
{
   // The part has stopped answering: BUSY never cleared or a transfer failed past retrying. What happens
   // next is up to the you: record the reason somewhere that survives, then reset, halt, or carry on
   // without storage.
   //
   // This is not expected to return. If it does, the caller continues with a chip that is not responding
}

void nandlog_port_unwritable(void)
{
   // The softer failure: the part still answers, but no block will retain a page. The log has already
   // disabled itself and stopped trying, so nothing here is urgent and nothing here should reset the system,
   // it just has nowhere to put data. Leave a breadcrumb the next boot can find
}
