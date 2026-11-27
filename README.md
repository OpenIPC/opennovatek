# opennovatek

Open, auditable sources for Novatek IP-camera SoCs, in the spirit of
[OpenIPC/openhisilicon](https://github.com/OpenIPC/openhisilicon).

Layout follows openhisilicon:

- `libraries/sensor/<chiparch>/<vendor>_<model>/`: image-sensor drivers built
  from vendor SDK source. For NA51089 these are kernel modules
  (`nvt_sen_*.ko`); see [libraries/sensor/na51089](libraries/sensor/na51089/README.md).
- `kernel/include/<chiparch>/`: the vendor SDK headers those drivers need.
- `bootrom/`: described below.

`bootrom/` is a clean-room reconstruction of the
on-chip mask ROM of NA51089 (NT98560, NT98561, NT98562, NT98566) as
reviewable C and ARM assembly. The vendor ROM image itself is never committed:
each contributor dumps it from their own camera, and `MANIFEST` pins the
sha256 the dump must match.

Why the ROM matters: it is what recovers a camera with a broken flash. It
decides where a loader may come from (SPI NOR/NAND, SD, Ethernet over DHCP
and TFTP, USB, UART), in what order, and what it checks before running one.
OpenIPC's [defib](https://github.com/OpenIPC/defib) and the ROM model in
[qemu-bench](https://github.com/OpenIPC/qemu-bench) were built from what this
reconstruction documents.

See [bootrom/README](bootrom/README) for the layout and the workflow.

Licensed under the GNU General Public License v3.0, as openhisilicon is.
