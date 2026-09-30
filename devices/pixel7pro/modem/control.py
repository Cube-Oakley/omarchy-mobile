#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Send a fixed local control action to the running physical-slot listener."""
import argparse
import socket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['status', 'provision-profile', 'allow-data', 'connect-data', 'connect-lte-access', 'connect-ims',
                                         'disconnect-data', 'disconnect-ims', 'start-ims', 'stop-ims',
                                         'register-ims', 'unregister-ims',
                                         'disable-nr', 'restore-nr',
                                         'data-centric', 'restore-usage', 'send-test-sms', 'send-test-ims-sms', 'network-auto'])
    args = parser.parse_args()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(120)
        client.connect('/run/modem-lab/control.sock')
        client.sendall(args.action.encode() + b'\n')
        result = client.recv(128)
        if result != b'OK\n':
            raise SystemExit('Action failed; inspect laboratory status log')
        print(args.action, 'accepted')


if __name__ == '__main__':
    main()
