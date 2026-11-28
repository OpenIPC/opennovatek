# na51089 sensor drivers

Image-sensor drivers for Novatek NA51089 (NT98560, NT98561, NT98562,
NT98566), imported from the vendor Linux SDK
`na51089_linux_sdk_uclibc_v1.01.002` (`code/hdal/ext_devices/sensor`)
and built here outside it, plus an open-source SC430AI driver that is not
from the SDK (see [SC430AI](#sc430ai)).

Unlike HiSilicon's userspace `libsns_*.so`, a Novatek sensor driver is a
kernel module. It registers itself with the HDAL capture framework
(`ctl_sen_reg_sendrv("nvt_sen_<model>", ...)`) and gets its board wiring
(I2C bus and address, reset/power-down GPIOs, MCLK, lane map) from the
device tree or from a `.cfg` file passed as `sen_cfg_path=`.

## Layout

    common/                 sen_cfg.c, sen_common.c, sen_i2c.c + headers,
                            compiled into every module (the SDK symlinked them)
    common/sensor.kbuild    the Kbuild every sensor directory includes
    common/sensor.mk        the out-of-tree make driver
    <vendor>_<model>/
        sen_<model>.c       the driver, as the SDK ships it (see Changes)
        Kbuild, Makefile    two lines and one line
        configs/            the SDK's EVB wiring presets for this sensor:
                            sen_<model>[_565|_56x].cfg and the matching
                            <model>_cfg[_565|_56x].dtsi (they differ in
                            I2C bus and reset GPIO)

The HDAL/VOS headers the drivers include (35 of them, the exact closure the
compiler reads) are in [`kernel/include/na51089`](../../../kernel/include/na51089).
`<plat/nvt-gpio.h>` comes from the Novatek kernel tree itself.

Module names stay `nvt_sen_<model>.ko`, as in the SDK, so a module built here
replaces the vendor one in an HDAL rootfs without touching the load scripts.

## Build

You need a Novatek 4.19.91 kernel tree, configured for na51089 and through
`make modules_prepare`: OpenIPC's
[`novatek-nt9856x`](https://github.com/openipc/linux/tree/novatek-nt9856x)
branch with an OpenIPC firmware `nt9856x` config, or the SDK's
`BSP/linux-kernel` with `na51089_evb_defconfig_nor_release`.

    make -C libraries CHIPARCH=na51089 KERNELDIR=/path/to/linux \
         CROSS_COMPILE=arm-linux-gnueabihf-

or one sensor at a time:

    make -C libraries/sensor/na51089/smart_sc500ai KERNELDIR=... CROSS_COMPILE=...

On a current host, 4.19's own build needs two nudges: `HOSTCFLAGS=-fcommon`
(its dtc predates GCC 10), and `CFG80211`/`SYSTEM_TRUSTED_KEYRING` off
(`extract-cert` does not link against OpenSSL 3+). The CI job in
`.github/workflows/build.yml` does exactly this.

The tree builds warning-free with the SDK's GCC 8.4 (uClibc) and with
GCC 13. As imported (commit a6c28df, before the fixes listed under Changes),
built with Buildroot GCC 8.4 against the OpenIPC kernel and config,
`sc401ai`, `sc500ai`, `sc501ai` and `imx415` have `.text`, `.rodata` and
`.data` byte for byte identical to the vendor modules OpenIPC firmware
shipped; only `.modinfo` (`depends=`) and the compiler banner differ.

The modules resolve `ctl_sen_*`, `isp_builtin_*` and `kdrv_builtin_*` at
load time from the vendor kflow/kdrv modules, which stay binary.

## Drivers

| Directory | Module | Vendor version |
|---|---|---|
| `aptina_ar0237` | `nvt_sen_ar0237.ko` | 1.43.000 |
| `aptina_ar0237ir` | `nvt_sen_ar0237ir.ko` | 1.43.000 |
| `galaxycore_gc4653` | `nvt_sen_gc4653.ko` | 1.45.000 |
| `galaxycore_gc4663` | `nvt_sen_gc4663.ko` | 1.44.000 |
| `galaxycore_gc4c33` | `nvt_sen_gc4c33.ko` | 1.44.000 |
| `galaxycore_gc5035` | `nvt_sen_gc5035.ko` | 1.44.000 |
| `jinglin_ti_module` | `nvt_sen_jinglin_ti_module.ko` | 1.42.000 |
| `omnivision_os02k10` | `nvt_sen_os02k10.ko` | 1.44.000 |
| `omnivision_os04a10` | `nvt_sen_os04a10.ko` | 1.45.000 |
| `omnivision_os04c10` | `nvt_sen_os04c10.ko` | 1.44.000 |
| `omnivision_os05a10` | `nvt_sen_os05a10.ko` | 1.46.000 |
| `omnivision_os08a10` | `nvt_sen_os08a10.ko` | 1.44.000 |
| `omnivision_os12d40` | `nvt_sen_os12d40.ko` | 1.44.000 |
| `omnivision_ov2715` | `nvt_sen_ov2715.ko` | 1.43.000 |
| `omnivision_ov2718` | `nvt_sen_ov2718.ko` | 1.43.000 |
| `omnivision_ov2736ir` | `nvt_sen_ov2736ir.ko` | 1.44.000 |
| `omnivision_ov2740` | `nvt_sen_ov2740.ko` | 1.44.000 |
| `omnivision_ov48b` | `nvt_sen_ov48b.ko` | 1.44.000 |
| `primesensor_ps5268` | `nvt_sen_ps5268.ko` | 1.43.000 |
| `samsung_s5k231y` | `nvt_sen_s5k231y.ko` | 1.43.000 |
| `smart_sc200ai` | `nvt_sen_sc200ai.ko` | 1.42.000 |
| `smart_sc2232h` | `nvt_sen_sc2232h.ko` | 1.43.000 |
| `smart_sc2239` | `nvt_sen_sc2239.ko` | 1.45.000 |
| `smart_sc2335` | `nvt_sen_sc2335.ko` | 1.44.000 |
| `smart_sc3335` | `nvt_sen_sc3335.ko` | 1.40.000 |
| `smart_sc401ai` | `nvt_sen_sc401ai.ko` | 1.46.000 |
| `smart_sc4210` | `nvt_sen_sc4210.ko` | 1.45.000 |
| `smart_sc4238` | `nvt_sen_sc4238.ko` | 1.43.000 |
| `smart_sc430ai` | `nvt_sen_sc430ai.ko` | not from the SDK |
| `smart_sc450ai` | `nvt_sen_sc450ai.ko` | 1.46.000 |
| `smart_sc500ai` | `nvt_sen_sc500ai.ko` | 1.44.000 |
| `smart_sc501ai` | `nvt_sen_sc501ai.ko` | 1.44.000 |
| `smart_sc8238` | `nvt_sen_sc8238.ko` | 1.44.000 |
| `soi_f35` | `nvt_sen_f35.ko` | 1.44.000 |
| `soi_f37` | `nvt_sen_f37.ko` | 1.44.000 |
| `soi_f37p` | `nvt_sen_f37p.ko` | 1.42.000 |
| `sony_imx230` | `nvt_sen_imx230.ko` | 1.44.000 |
| `sony_imx290` | `nvt_sen_imx290.ko` | 1.44.000 |
| `sony_imx307` | `nvt_sen_imx307.ko` | 1.43.000 |
| `sony_imx307_slave` | `nvt_sen_imx307_slave.ko` | 1.43.000 |
| `sony_imx317` | `nvt_sen_imx317.ko` | 1.43.000 |
| `sony_imx334` | `nvt_sen_imx334.ko` | 1.44.000 |
| `sony_imx335` | `nvt_sen_imx335.ko` | 1.46.000 |
| `sony_imx415` | `nvt_sen_imx415.ko` | 1.44.000 |

`jinglin_ti_module` is a vendor board module rather than a bare sensor;
it is kept under the SDK's own name.

Not imported: `sen_off` (an empty placeholder module), and the SDK's
`imx291`, `imx290_fpga` and `os02k10d` configs, which have no driver in
this SDK.

## SC501AI

Novatek has no separate SC501AI driver: the `nvt_sen_sc501ai.ko` OpenIPC
firmware shipped is `sen_sc500ai.c` with the name changed, and
`smart_sc501ai/Kbuild` generates its source the same way. Tested on an
NT98566 + SC501AI camera: in place of the vendor module it loads, streams
2880x1620 and leaves the same kernel log. With bare HDAL (no majestic),
select sensor mode 2 explicitly at 25 fps; `HD_VIDEOCAP_SEN_MODE_AUTO` does
not pick a working mode.

## SC430AI

The SDK has no SC430AI driver. `smart_sc430ai` is a clean-room GPL-2.0
driver by Vladimir Bely, from OpenIPC/firmware#2977, tested there on a Dahua
DH-IPC-HFW2449SP-S-IL (NT98566 + SC430AI): 2688x1520 at 30 and 60 fps
linear, mirror and flip. Its third mode, 2-frame stagger HDR, is untested.

It is self-contained: its own I2C access and `.cfg` parser instead of
`common/`, so no device-tree wiring, and its Kbuild does not include
`sensor.kbuild`. It is built against the SDK's HDAL headers like the rest.

Changes since that PR (commit b1ba5dc is the PR verbatim):

- The hand-written copies of the HDAL structs and ids are replaced by the
  SDK types. The generated code is unchanged except that the HDR mode now
  reports `CTL_SEN_MODE_STAGGER_HDR` (5); it reported 2, which is
  `CTL_SEN_MODE_BUILTIN_HDR`.
- The ISP's preset (`USER_DEFINE1`, an `ISP_SENSOR_PRESET_CTRL`) was read as
  the cfg preset, so a mode change after it started from a bogus exposure
  and gain. Presets now follow the SDK drivers.
- The MCLK callback got the sensor id instead of the configured clock;
  `GET_FPS` swapped its two fields.

## Changes against the SDK

Fixes made after import, from review (commit a6c28df is the SDK verbatim):

- `common/sen_cfg.c`: a key without `=` returned an uninitialised pointer;
  an empty value read one byte before the buffer; a value that filled the
  buffer had its terminator written one byte past it (continuation lines
  too).
- `common/sen_common.c`: the device-tree `path` indexed
  `CTL_SEN_ID_MAX`-sized arrays unchecked.
- `common/sen_i2c.c`: a failed I2C probe left `sen_i2c_info[id]` NULL while
  init reported success, so close and every transfer dereferenced NULL; the
  new client leaked when `i2c_add_driver` failed.
- `sony_imx290`, `sony_imx307`, `sony_imx307_slave`: the sensor-ID probe
  compared the ID before checking the I2C read, so IMX290 (ID 0) took a
  failed read for a match.
- `sony_imx307_slave/configs/*.dtsi`: the node was named `sen_imx307`, but
  the driver looks for `sen_imx307_slave`.
