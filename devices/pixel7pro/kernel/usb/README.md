# USB data-switch recovery

The MAX77759 Type-C controller opens its USB2 data switch when the cable is
removed. The bootloader reconnects it, but the inherited peripheral-only
Linux setup did not. This looked like a suspend failure: the phone and Wi-Fi
worked, USB detected power, and DWC3 remained `not attached` after replugging.

An 83.972-second unplugged sleep reproduced it. Gadget reconnect, DWC3 rebind
and a USB2 PHY reset did not help. The TCPC reported the expected vendor and
product IDs, present VBUS, and `USBSW_CTRL=0`. Writing `0x09` restored USB
enumeration immediately without restarting. This is the setting used by
Google's [enable_data_path_locked](https://android.googlesource.com/kernel/gs/+/refs/heads/android-gs-pantah-5.10-android13-d1/drivers/usb/typec/tcpm/google/tcpci_max77759.c)
and [register definitions](https://android.googlesource.com/kernel/gs/+/refs/heads/android-gs-pantah-5.10-android13-d1/drivers/usb/typec/tcpm/google/tcpci_max77759_vendor_reg.h).

`pixel-usb-switch` claims address 0x25 on the battery driver's `Pixel hsi2c_13`
adapter, verifies the chip IDs, and restores only a disconnected switch with
VBUS present. It writes no PD, CC, role or charging controls. It refuses an
address already owned by a full TCPC driver. Unknown switch routes are kept.

The existing battery supply notifier handles insertion, and resume schedules
a check. A freezable worker also checks every two seconds while VBUS is
present, covering unplug/replug between battery polls. It stops when unplugged
and does not run during system suspend. The read-only `reconnects` module
parameter counts successful repairs. Unloading leaves the connection usable.

The helper loads after the battery driver. New recovery images include it;
an installed `/usr/local/lib/omarchy-mobile/pixel-usb-switch.ko` takes priority.
Build with the matching kernel and `M=$PWD/devices/pixel7pro/kernel/usb`.

Validation: normal boot loaded the helper. Three controlled writes recreating
the observed disconnected switch each returned to `configured` automatically,
with repair counts 1, 2 and 3. A subsequent 8.725-second RTC sleep resumed with
USB configured and Wi-Fi SSH working. A physical cable test of the automatic helper
is still outstanding; the original physical failure was recovered manually.

## Wi-Fi recovery access

`scripts/pixel-wifi-ssh.py` installs as `/usr/local/sbin/pixel-wifi-ssh`.
`enable ADDRESS` requires that IPv4 address on a Wi-Fi interface and persists
the choice locally. A separate SSH listener uses the existing authorized key
and host key, with password authentication and forwarding disabled. USB SSH
remains independent. `disable` removes the policy and stops the listener.

The boot helper waits up to two minutes for Wi-Fi DHCP; a NetworkManager
dispatcher handles subsequent address changes. There is no permanent polling.
The configured address must stay assigned; a different DHCP address requires
enabling the new address. Authenticated Wi-Fi access survived a normal reboot.
