# DSCD Scheduler

This repository contains the DSCD scheduler as a Linux kernel module and its iproute2/tc module.
The tc module is used to configure the DSCD scheduler from userspace.

## Installation

First, install the Linux kernel header files.
Then, compile the kernel module and tc module.

```bash
# Build DSCD kernel module
$ cd dscd_scheduler/
$ make
$ sudo insmod sch_dscd.ko # Load the kernel module into the running kernel (root required)

cd ..

# Build tc module
$ git submodule update --init --recursive # clone iproute2 submodule
$ cd dscd_tc/
$ ./build.sh # Creates the tc_lib/ directory
```

## Usage

To use the loaded scheduler, it must be configured with `tc`.

The tc command can be extended with dynamic libraries.
The environment variable `TC_LIB_DIR` is used to specify the directory containing all tc modules as dynamic libraries (*.so).
The last command `./build.sh` creates a directory `tc_lib`, which contains the dynamic library `q_dscd.so`.

Assuming you already have installed iproute2:
```bash
# Environment variable as prefix:
$ TC_LIB_DIR=tc_lib tc qdisc show

# or export environment variable:
$ export TC_LIB_DIR=tc_lib
$ tc qdisc show
```

### Configuration

To list all available configuration options (Replace `IFACE` with the interface name):
```bash
$ TC_LIB_DIR=tc_lib tc qdisc add dev IFACE dscd help

# or

$ export TC_LIB_DIR=tc_lib
$ tc qdisc add dev IFACE dscd help


# Output:
Usage: ... dscd [ B_max SIZE ] [ C RATE ]
                [ credit_half_life TIME ] [ rate_memory TIME ]
                [ T_d TIME ] [ T_q NUM ]
```

Configuration example (root required):

| Option           | Value                      |
|------------------|----------------------------|
| B_max            | 3.125 MB                   |
| C                | 0 (= bandwidth estimation) |
| credit_half_life | 100 ms                     |
| rate_memory      | 50 ms                      |
| T_d              | 10 ms                      |
| T_q              | 1                          |

```bash
$ TC_LIB_DIR=tc_lib tc qdisc add dev IFACE root dscd B_max 3125000 C 0 credit_half_life 1s rate_memory 50ms T_d 2ms T_q 2
```

### Statistics

`tc` can also be used to show qdisc configuration options and statistics:

```bash
$ TC_LIB_DIR=tc_lib tc qdisc show dev IFACE       # print only configuration options
qdisc dscd 8000: root B_max 3125000b C 0bit credit_half_life 1s rate_memory 50ms T_d 2ms T_q 2
 Sent 8065663614 bytes 5327642 pkt (dropped 8942, overlimits 0 requeues 0)
 backlog 3047658b 2014p requeues 0


$ TC_LIB_DIR=tc_lib tc -s qdisc show dev IFACE    # -s prints qdisc statistics
qdisc dscd 8000: root B_max 3125000b C 0bit credit_half_life 1s rate_memory 50ms T_d 2ms T_q 2
 Sent 8065663614 bytes 5327642 pkt (dropped 8942, overlimits 0 requeues 0)
 backlog 3047658b 2014p requeues 0

rate 1Gbit
weighted rate sum 6260901
weighted rate count 50069854

                            ABE           BE      Service
  length                      1         2011         2012
  credit                      0            0      3046144

                            ABE           BE          ALL
  sum delay               9.03s    8.69e+04s    8.69e+04s
  recv packets              575      5329079      5329654
  sent packets              574      5327068      5327642
  enqueue drops               1         8941         8942
  dequeue drops               0            0            0
  avg delay              15.7ms       16.3ms       16.3ms

```


If you have any questions, feel free to [contact me](mailto:gabriel.paradzik@uni-tuebingen.de).