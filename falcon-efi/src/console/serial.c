/* falcon-boot: lines for the serial port only. See serial.h. */

#include <console/serial.h>
#include <efi/serialio.h>

static void put(EFI_SERIAL_IO_PROTOCOL *io, const char *s, UINTN n)
{
    UINTN len = n;

    (void)io->Write(io, &len, (void *)(uintptr_t)s);
}

void serial_line(EFI_SYSTEM_TABLE *st, const char *s)
{
    EFI_GUID guid = EFI_SERIAL_IO_PROTOCOL_GUID;
    EFI_SERIAL_IO_PROTOCOL *io = (EFI_SERIAL_IO_PROTOCOL *)0;
    UINTN n = 0;

    if (st == (EFI_SYSTEM_TABLE *)0 || s == (const char *)0 ||
        st->BootServices == (EFI_BOOT_SERVICES *)0)
        return;
    if (st->BootServices->LocateProtocol(&guid, (void *)0, (void **)&io) !=
            EFI_SUCCESS ||
        io == (EFI_SERIAL_IO_PROTOCOL *)0 ||
        io->Write == (void *)0)
        return;
    while (s[n] != '\0')
        n++;
    /* The CR LF ahead of it starts a line wherever the console's own
     * output on the same port left the cursor. */
    put(io, "\r\n", 2u);
    put(io, s, n);
    put(io, "\r\n", 2u);
}
