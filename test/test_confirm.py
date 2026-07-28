# SPDX-License-Identifier: LGPL-2.1-only
# SPDX-FileCopyrightText: 2025 Pengutronix

from pexpect import TIMEOUT, EOF

from helper import run_pexpect


def test_confirm_success(hawkbit, confirm_config, bundle_assigned,
                         rauc_dbus_install_success_scenario):
    """
    With confirm_after_reboot enabled, a bundle that installs and then boots into the target
    slot marked good must be reported as proceeding first and success only after the
    (simulated) reboot. The pending state file must be cleaned up afterwards.
    """
    config, data_dir = confirm_config
    rauc_dbus_install_success_scenario(reboot_to='rootfs.1', reboot_boot_status='good')

    proc = run_pexpect(f'rauc-hawkbit-updater -c "{config}"')

    proc.expect('Software bundle installed, confirming after reboot')
    proc.expect('Update confirmed after reboot')

    # let feedback propagate to hawkBit before termination
    proc.expect(TIMEOUT, timeout=2)
    proc.terminate(force=True)
    proc.expect(EOF)

    assert not (data_dir / 'pending-confirmation').exists()

    status = hawkbit.get_action_status()
    assert status[0]['type'] == 'finished'


def test_confirm_rollback(hawkbit, confirm_config, bundle_assigned,
                          rauc_dbus_install_success_scenario):
    """
    If the system boots back into the previous slot (rollback) after the install, the update
    must be reported as failure and the pending state cleaned up.
    """
    config, data_dir = confirm_config
    # target is rootfs.1 (primary), but the system stays on / rolls back to rootfs.0
    rauc_dbus_install_success_scenario(reboot_to='rootfs.0')

    proc = run_pexpect(f'rauc-hawkbit-updater -c "{config}"')

    proc.expect('Software bundle installed, confirming after reboot')
    proc.expect('Update rolled back to previous slot after reboot')

    proc.expect(TIMEOUT, timeout=2)
    proc.terminate(force=True)
    proc.expect(EOF)

    assert not (data_dir / 'pending-confirmation').exists()

    status = hawkbit.get_action_status()
    assert status[0]['type'] == 'error'


def test_confirm_pending(hawkbit, confirm_config, bundle_assigned,
                         rauc_dbus_install_success_scenario):
    """
    While the target slot is booted but not yet marked good, no final feedback must be sent,
    the pending state must be retained and the still-open deployment must not be reinstalled.
    """
    config, data_dir = confirm_config
    rauc_dbus_install_success_scenario(reboot_to='rootfs.1', reboot_boot_status='bad')

    proc = run_pexpect(f'rauc-hawkbit-updater -c "{config}"')

    proc.expect('Software bundle installed, confirming after reboot')
    # verdict stays undecided while the target slot is not yet marked good
    proc.expect('not yet confirmed')
    # the still-open offer must be ignored rather than reinstalled
    proc.expect('Ignoring deployment offer while an update confirmation is pending')

    # no final verdict must be reported
    assert proc.expect(['Update confirmed after reboot',
                        'Update rolled back to previous slot after reboot',
                        TIMEOUT], timeout=10) == 2

    proc.terminate(force=True)
    proc.expect(EOF)

    assert (data_dir / 'pending-confirmation').exists()

    status = hawkbit.get_action_status()
    assert status[0]['type'] != 'finished'
