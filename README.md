# Raspberry Pi Pico Persistent Circular Data Logger

This project demonstrates the use of a circular buffer on the Raspberry Pi Pico's onboard flash memory. A circular buffer is a data structure that cyclically uses a fixed-size array to store data. Data is added sequentially, and the oldest data is overwritten by the newest entries. This structure is particularly useful for resource-constrained microcontrollers to maintain sensor data over a period. The implementation persists data in flash memory, making it resilient to power disruptions. Additionally, it features APIs to traverse the data in both ascending and descending order.

The demonstration will continue to record temperature sensor data with elapsed time.


## Important Limitations

This implementation persistently writes to and erases the same areas of flash memory to store data. As a result, if power is lost before the flash memory area has been erased and rewritten during the addition of new data, data loss may occur. This limitation is important to consider when using the data logger in environments where power supply may be unstable.

## Build and Install

```bash
git submodule update --init
cd lib/pico-sdk; git submodule update --init; cd ../../
mkdir build; cd build
cmake ..
make
```

Once built, the `circularbuffer.uf2` file can be dragged and dropped onto your Raspberry Pi Pico to install and run the example.

## Testing

The tests directory contains code to verify the API's behavior. After building and installing the test code on the Pico, the unit tests are executed directly on the device, and results are sent via UART.

```
make tests
```

To run the tests, transfer the tests/tests.uf2 file to your Pico. For a more comprehensive debugging experience, connect the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) to the SWD Debug and UART Serial interfaces of the Pico. Use the following command to install and run the tests:

```
make run_tests
```


