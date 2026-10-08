# SCSI generic (sg.ko)

Our kernel has `CONFIG_CHR_DEV_SG` off. Trusty's `storageproxyd` reaches the
UFS RPMB well-known LUN (SCSI 0:0:0:49476) through SCSI generic, as stock's
`/dev/sg1` (`fingerprint/fp-lab.sh --storage`). The module is mainline's
`drivers/scsi/sg.c`, unchanged, built out of tree. The source is not tracked:

```
cp mainline/linux/drivers/scsi/sg.c kernel/scsi-sg/
make -C mainline/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$PWD/kernel/scsi-sg modules
```

Loaded, it numbers the LUNs as stock does (October 1): sg0 the UFS device
W-LUN, sg1 RPMB, sg2 boot, sg3-6 LUNs 0-3. SG_IO filters commands only on
nodes opened read-only, so storageproxyd needs no capability for RPMB.
