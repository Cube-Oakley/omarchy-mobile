# SCSI generic (drivers/scsi/sg.c, unchanged) for the UFS RPMB LUN, which
# storageproxyd reaches through /dev/sgN; CONFIG_CHR_DEV_SG is off in our kernel.
obj-m += sg.o
ccflags-y += -I$(srctree)/drivers/scsi
