#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Manual Tello data control; requires prior radio/data authorization."""
import argparse
from data_call import activate, deactivate, initial_attach
from sit import Sit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apn', choices=['wholesale', 'fast.t-mobile.com'], default='wholesale')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--initial-attach', action='store_true')
    action.add_argument('--disconnect', action='store_true')
    args = parser.parse_args()
    client = Sit()
    try:
        if args.initial_attach:
            initial_attach(client, args.apn)
        elif args.disconnect:
            deactivate(client)
        else:
            activate(client, args.apn)
    finally:
        client.close()


if __name__ == '__main__':
    main()
