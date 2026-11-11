# mapple_loader

The boot loaders: `shim/`, viva's shim, which LK starts in place of a Linux kernel and which starts XNU
(single boot); `mtk/loader/`, mtkloader, which starts either Android's kernel or XNU from the same boot image
(dual boot), sends the touch firmware and passes the phone's own tables to the kernel.

Checked out at `mapple/loader` by the [Open Mapple manifest](https://github.com/open-mapple/manifest), which says how to get, build and install it.

Licence: MIT (`LICENSE`, `NOTICE`).
