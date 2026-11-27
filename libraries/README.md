# libraries

Per-SoC drivers built from vendor SDK source, laid out like
[openhisilicon](https://github.com/OpenIPC/openhisilicon)'s `libraries/`.

    make CHIPARCH=na51089 KERNELDIR=/path/to/linux CROSS_COMPILE=arm-linux-gnueabihf-

| CHIPARCH | SoCs | What |
|---|---|---|
| `na51089` | NT98560, NT98561, NT98562, NT98566 | [sensor drivers](sensor/na51089/README.md) (kernel modules) |
