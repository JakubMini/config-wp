# config-wp

Skeleton project for the Rivan embedded configuration management exercise.

The goal of the exercise is to design and implement a configuration
management system suitable for our embedded systems. This repository
provides the development environment so that work can be done on any
machine, run under FreeRTOS (POSIX port), and unit-tested with
GoogleTest / GoogleMock.

## Layout

```
.
├── CMakeLists.txt          Root build (FreeRTOS app + tests)
├── cmake/FreeRTOSConfig.h  FreeRTOS kernel config
├── src/
│   ├── main.c              Scheduler bring-up + app task
│   ├── application/        Configuration management code (your work)
│   └── drivers/            Hardware abstractions (storage, etc.)
├── tests/                  GTest / GMock unit tests
├── Dockerfile              Builds + runs the FreeRTOS binary
└── Dockerfile.test         Builds + runs the unit tests
```

`application/` and `drivers/` are intentionally minimal - extend them.

## Run the application

```
docker build -t config-wp .
docker run --rm -it config-wp
```

## Run the tests

```
docker build -f Dockerfile.test -t config-wp-test .
docker run --rm config-wp-test
```
## System-level details + constraints:
* Ultimately will be deployed on STM32Gx w/ FreeRTOS. For demonstration purposes no need to run on target. Running on a host (clang w/ POSIX port of FreeRTOS preferred) is fine.
* Must be relatively simple to port code to a new HW vendor. No need for OS abstraction layer, presume we will continue to use FreeRTOS.
* C or C++, at your discretion
* Must have reasonable test coverage - pref GTest/Gmock

## Requirements
* Stores configuration data needed to operate IOs associated with the system
  * DI (name: string(16), ID, debounce time, fault state: hold/0/1…)
  * DO (name,...)
  * TC (name,...)
  * AI (name,...)
  * AO (name,...)
  * PCNT (name,...)
  * PWM (name, period, duty, fault state: hold/off/on…)
* We are considering abiding by the CiA 401 device profile (generic I/O device), so expect these parameters to align pretty well with it. PDF available here after making an account: https://www.can-cia.org/can-knowledge/cia-401-series-i/o-device-profile
  * The profile will require some extension to define counter input and PWM output
* Stores configuration data required for the system to run (e.g. CAN ID, data rate…) 
* R/W access to the configuration must be re-entrant / thread safe 
* Configuration format must support future extensions and modifications, and configuration must remain forward compatible
* Configuration should be flexible - i.e. some I/Os may be dual/triple function
* Configuration storage must be abstracted for the medium: may be stored on internal flash, external EEPROM etc. 
* Configuration must be (relatively) human readable / editable
* Configuration must be modifiable piece-wise or monolithically (i.e. changing config on a per-I/O basis, or as a whole device)
* Manager must be able to detect R/W/input errors and validate inputs