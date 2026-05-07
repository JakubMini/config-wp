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

`application/` and `drivers/` are intentionally minimal — extend them.

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

