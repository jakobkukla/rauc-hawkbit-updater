#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
# SPDX-FileCopyrightText: 2021-2025 Bastian Krause <bst@pengutronix.de>, Pengutronix

import hashlib
import time
from pathlib import Path

from gi.repository import GLib
from pydbus.generic import signal
import requests


class Installer:
    """
    D-Bus interface `de.pengutronix.rauc.Installer`, to be used with
    `pydbus.{SessionBus,SystemBus}.publish()`

    The interface is defined via the xml read in the dbus class property.
    The relevant D-Bus properties are implemented as Python's @property/@Progress.setter.
    The D-Bus methods are Python methods.

    See https://github.com/LEW21/pydbus/blob/master/doc/tutorial.rst#class-preparation
    """
    dbus = Path('../src/rauc-installer.xml').read_text()
    interface = 'de.pengutronix.rauc.Installer'

    Completed = signal()
    PropertiesChanged = signal()

    def __init__(self, bundle, completed_code=0, *, primary='rootfs.1', boot_slot='rootfs.0',
                 slots=None, reboot_to=None, reboot_boot_status=None):
        self._bundle = bundle
        self._completed_code = completed_code

        self._operation = 'idle'
        self._last_error = ''
        self._progress = 0, '', 1

        # slot state used by GetPrimary/GetSlotStatus/BootSlot (confirm_after_reboot)
        self._primary = primary
        self._boot_slot = boot_slot
        self._slots = slots or {
            'rootfs.0': {'device': '/dev/sda2', 'boot-status': 'good',
                         'installed.transaction': '0000-0000-old'},
            'rootfs.1': {'device': '/dev/sda3', 'boot-status': 'good',
                         'installed.transaction': '1111-1111-new'},
        }
        # simulate the effect of a reboot once an install completes: switch the booted slot
        # (and optionally its boot-status) to model committing to / rolling back from the
        # freshly installed slot
        self._reboot_to = reboot_to
        self._reboot_boot_status = reboot_boot_status

    def InstallBundle(self, source, args):
        def mimic_install():
            """Mimics a sucessful/failing installation, depending on `self._completed_code`."""
            progresses = [
                'Installing',
                'Determining slot states',
                'Determining slot states done.',
                'Checking bundle',
                'Verifying signature',
                'Verifying signature done.',
                'Checking bundle done.',
                'Loading manifest file',
                'Loading manifest file done.',
                'Determining target install group',
                'Determining target install group done.',
                'Updating slots',
                'Checking slot rootfs.1',
                'Checking slot rootfs.1 done.',
                'Copying image to rootfs.1',
                'Copying image to rootfs.1 done.',
                'Updating slots done.',
                'Install failed.' if self._completed_code else 'Installing done.',
            ]

            self.Operation = 'installing'

            for i, progress in enumerate(progresses):
                percentage = (i+1)*100 / len(progresses)
                self.Progress = percentage, progress, 1
                time.sleep(0.1)

            # simulate the reboot into (or rollback from) the freshly installed slot
            if not self._completed_code and self._reboot_to:
                self._boot_slot = self._reboot_to
                if self._reboot_boot_status:
                    self._slots[self._reboot_to]['boot-status'] = self._reboot_boot_status

            self.Completed(self._completed_code)

            if not self._completed_code:
                self.LastError = 'Installation error'

            self.Operation = 'idle'

            # do not call again
            return False

        print(f'installing {source}')
        try:
            self._check_install_requirements(source, args)
        except Exception as e:
            self.Completed(1)
            self.LastError = f'Installation error: {e}'
            self.Operation = 'idle'
            raise

        GLib.timeout_add_seconds(interval=1, function=mimic_install)

    @staticmethod
    def _get_bundle_sha1(bundle):
        """Calculates the SHA1 checksum of `self._bundle`."""
        sha1 = hashlib.sha1()

        with open(bundle, 'rb') as f:
            while True:
                chunk = f.read(sha1.block_size)
                if not chunk:
                    break
                sha1.update(chunk)

        return sha1.hexdigest()

    @staticmethod
    def _get_http_bundle_sha1(url, *, auth_header=None, cert=None, verify=True):
        """Download file from URL using HTTP range requests and compute its sha1 checksum."""
        sha1 = hashlib.sha1()
        headers = auth_header or {}
        range_size = 128 * 1024  # default squashfs block size

        offset = 0
        while True:
            headers['Range'] = f'bytes={offset}-{offset + range_size - 1}'
            r = requests.get(url, headers=headers, cert=cert, verify=verify)
            try:
                r.raise_for_status()
                sha1.update(r.content)
            except requests.HTTPError:
                if r.status_code == 416:  # range not satisfiable, assuming download completed
                    break
                raise

            offset += range_size

        return sha1.hexdigest()

    def _check_install_requirements(self, source, args):
        """
        Check that required headers are set, bundle is accessible (HTTP or locally) and its
        checksum matches.
        """
        if 'tls-key' in args and 'tls-cert' in args:
            cert = (args['tls-cert'], args['tls-key'])
            if 'tls-no-verify' in args and args['tls-no-verify']:
                verify = False
            elif 'tls-ca' in args:
                verify = args['tls-ca']
            else:
                verify = True

            source_sha1 = self._get_http_bundle_sha1(source, cert=cert, verify=verify)

        elif 'http-headers' in args:
            assert len(args['http-headers']) == 1

            [auth_header] = args['http-headers']
            key, value = auth_header.split(': ', maxsplit=1)
            source_sha1 = self._get_http_bundle_sha1(source, auth_header={key: value})

            # assume ssl_verify=false is set in test setup
            assert args['tls-no-verify'] is True
        else:
            source_sha1 = self._get_bundle_sha1(source)

        # check bundle checksum matches expected checksum
        assert source_sha1 == self._get_bundle_sha1(self._bundle)

    @property
    def Operation(self):
        return self._operation

    @Operation.setter
    def Operation(self, value):
        self._operation = value
        self.PropertiesChanged(Installer.interface, {'Operation': self.Operation}, [])

    @property
    def Progress(self):
        return self._progress

    @Progress.setter
    def Progress(self, value):
        self._progress = value
        self.PropertiesChanged(Installer.interface, {'Progress': self.Progress}, [])

    @property
    def LastError(self):
        return self._last_error

    @LastError.setter
    def LastError(self, value):
        self._last_error = value
        self.PropertiesChanged(Installer.interface, {'LastError': self.LastError}, [])

    def GetPrimary(self):
        return self._primary

    def GetSlotStatus(self):
        result = []
        for name, status in self._slots.items():
            slot = {key: GLib.Variant('s', value) for key, value in status.items()}
            # RAUC marks exactly the booted slot with state "booted"
            slot['state'] = GLib.Variant('s', 'booted' if name == self._boot_slot
                                         else 'inactive')
            result.append((name, slot))
        return result

    @property
    def Compatible(self):
        return "not implemented"

    @property
    def Variant(self):
        return "not implemented"

    @property
    def BootSlot(self):
        return self._boot_slot


if __name__ == '__main__':
    import argparse
    from pydbus import SessionBus

    parser = argparse.ArgumentParser()
    parser.add_argument('bundle', help='Expected RAUC bundle')
    parser.add_argument('--completed-code', type=int, default=0,
                        help='Code to emit as D-Bus Completed signal')
    parser.add_argument('--primary', default='rootfs.1',
                        help='Slot returned by GetPrimary (the install target)')
    parser.add_argument('--boot-slot', default='rootfs.0',
                        help='Initial BootSlot (slot the system booted from)')
    parser.add_argument('--reboot-to', default=None,
                        help='On a successful install, switch BootSlot to this slot to '
                             'simulate the reboot into (or rollback from) the update')
    parser.add_argument('--reboot-boot-status', default=None, choices=('good', 'bad'),
                        help='boot-status of --reboot-to slot after the simulated reboot')
    args = parser.parse_args()

    loop = GLib.MainLoop()
    bus = SessionBus()
    installer = Installer(args.bundle, args.completed_code, primary=args.primary,
                          boot_slot=args.boot_slot, reboot_to=args.reboot_to,
                          reboot_boot_status=args.reboot_boot_status)
    with bus.publish('de.pengutronix.rauc', ('/', installer)):
        print('Interface published')
        loop.run()
