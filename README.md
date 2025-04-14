# Raspberry Pi Pico Persistent ringbuffer

This project started with the excellent example
https://github.com/oyama/pico-persistent-circular-buffer/blob/main/README.md

I wanted a more flexible implementation that would hide the internal flash
file system details, but allow the user code much more flexibility. It allows
dynamically sized entries of multiple types (with an id), which can be
searched by the calling code. It also allows flash entries to be deleted,
without changing anything else in the flash to be affected.

Flash is precious and limited. It can wear out with too many erase cycles
(about 100,000 for the pico w flash). Also multiple, independent ringbuffers
can be used in a linker protected (from code overflow) flash area. I hope
this code can be easily reused on any flash based system with minor changes.

This project's main.c demonstrates the use of a flexible ringbuffer on the
Raspberry Pi Pico's onboard flash memory. Data is added sequentially, and the
oldest data (if requested by caller) is overwritten by the newest entries.
The implementation persists data in flash memory, making it resilient to
power disruptions. Additionally, it features APIs to add data with an id and
read data with a matching id, and delete data that matches an id and a search
string. I have in mind a system that holds ssid/password pairs and also
allows holding somewhat dynamic json or html. It can also have logging data
with either fixed or dynamic lengths.

## NOR flash info

To find specs on the flash on the current rpi Pico W search for W25Q16JVl and
read the datasheet. Summary, at least 100,000 program-erase cycles per
sector. At least 20 year retention. Sectors are 4k blocks, which are all
erased with a command to hex value 0xff. This NOR part allows writing from 1
to 256 bytes per 256 byte aligned PAGE. The Pico sdk incorrectly requires
always writing a full page, so that is what this ringbuffer code does.

NOR flash actually erases a sector to all 0xff, and subsequent writes clear
zero bits. So by pre-padding a page to all 0xff allows byte by byte or bit by
bit writing. Overwriting previously written bit/byte(s) with 0xff does not
actually change the previous written or erased data. Only zero bits change
the flash data.

## Data maximum size

The ring buffer (which can be from 1 to n flash sectors), is automatically
maintained. Any write can split over 2 sectors and the following read will
automatically recombine the data. So the maximum allowed append is one sector
minus 2 header overhead or RB_MAX_APPEND_SIZE == 4096-4-4 bytes. Anything
longer gets really complicated to recombine and return to the caller. If the
ringbuffer overflows and a wrapped sector is deleted, the application must
detect that a short read occurred and handle it. Fixed sized id entries make
this easy, but maybe the original circular buffer would be more efficient in
flash usage, having lower system overhead if only fixed sizes are used.

A single sector ringbuffer is problematic. If an append overflows, only the
last of the data will be written. Also there is a risk that a power fail
could lose new unwritten data. If you use a single sector, I recommend not
automatically wrapping, but rather failing on an append that would wrap. Then
the user could choose what to do.

Having multiple flash storage areas can simplify things like the speculated
ssid/password areas and json data areas. Possible flash availability in a rpi
Pico W is limited by the application code as well as this sort of ringbuffer
areas. The linker can detect overflow during the build if you use a
custom .ld file as in this main.c example.

## Warning

I have tested this code, but not every edge case. Especially problematic are
random hardware errors, like a read dropping bits. This can happen on
over-used or really old flash. Or buggy ringbuffer code. Since the code
implements a kind of simple file system, errors in the underlying file system
can result in reads or writes getting in some kind of loop which can hang the
code. This happens especially when modifying the ringbuffer code and testing.
I have found several of these problems and try to detect loops, but probably
some bugs remain.

This code works for me (so far) your code will need testing.

Your mileage may vary.

I have added a couple of higher level user routines as examples for usage. I
changed the API so all functions return a negative error code, or zero if ok,
or greater than zero if the function returns.

## Build and Install

This now assumes you have the pico-sdk installed and have defined in your environment PICO_SDK_PATH

```bash
mkdir build; cd build
cmake ..
make
```

Once built, the `ringbuffer.uf2` file can be dragged and dropped onto your Raspberry Pi Pico to install and run the example.

## Testing

I only have a main.c file which can be edited for testing. It is not complete. More testing is needed.

