# Raspberry Pi Pico Fixed Length Data Logger Example

This project is a working demonstration of a persistent circular buffer using the Raspberry Pi Pico's onboard flash memory.
The circular buffer can go back from the latest data to the past and from the oldest data to the newest data. Since it is a circular buffer, the storage data consumption is constant.

The demonstration will continue to record temperature sensor data with elapsed time.

```bash
git submodule update --init
cd lib/pico-sdk; git submodule update --init; cd ../../
mkdir build; cd build
cmake ..
make
```

