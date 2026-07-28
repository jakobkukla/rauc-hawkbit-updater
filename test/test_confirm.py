# SPDX-License-Identifier: LGPL-2.1-only
# SPDX-FileCopyrightText: 2025 Pengutronix

import os

import pytest
from pexpect import TIMEOUT, EOF

from helper import run, run_pexpect


@pytest.fixture
def preload_fake_reboot(monkeypatch):
    """
    Preload the meson-built reboot()-neutralizing shim into processes spawned by this test,
    so the confirm flow runs through install_complete_cb's reboot() without rebooting the
    host (safe even as root with CAP_SYS_BOOT). Requesting the fixture applies it;
    monkeypatch unsets LD_PRELOAD again after the test. Defined here so it is scoped to the
    confirm tests only.
    """
    lib = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                                       'build', 'fake_reboot.so'))
    # Fail rather than skip: the shim is built by meson alongside the updater, so its
    # absence means a broken build. Skipping would silently drop confirm coverage; running
    # without it would leave reboot() live (a real reboot on a root/CAP_SYS_BOOT runner).
    if not os.path.exists(lib):
        pytest.fail(f'reboot shim not built: {lib} (build the project with meson first)')
    monkeypatch.setenv('LD_PRELOAD', lib)


def test_confirm_requires_post_update_reboot(adjust_config, tmp_path):
    """confirm_after_reboot without post_update_reboot must be rejected at config load."""
    data_dir = tmp_path / 'data'
    data_dir.mkdir()
    config = adjust_config({'client': {
        'confirm_after_reboot': 'true',
        'post_update_reboot': 'false',
        'data_directory': str(data_dir),
    }})

    out, err, exitcode = run(f'rauc-hawkbit-updater -c "{config}"')

    assert exitcode == 4
    assert "'post_update_reboot' is required if 'confirm_after_reboot' is enabled" in err


def test_confirm_success(hawkbit, confirm_config, bundle_assigned,
                         rauc_dbus_install_success_scenario, preload_fake_reboot):
    """
    Full flow: booted on the old slot, install to the target, report proceeding, reboot
    (neutralized) into the target slot marked good -> confirm as success, state cleaned up.
    """
    config, data_dir = confirm_config
    rauc_dbus_install_success_scenario(reboot_to='rootfs.1', reboot_boot_status='good')

    proc = run_pexpect(f'rauc-hawkbit-updater -c "{config}"')

    # install side ran: proceeding reported and the pending state persisted
    proc.expect('Software bundle installed, confirming after reboot')
    assert (data_dir / 'pending-confirmation').exists()
    # verdict after the (neutralized) reboot into the target slot
    proc.expect('Update confirmed after reboot')

    # let feedback propagate to hawkBit before termination
    proc.expect(TIMEOUT, timeout=2)
    proc.terminate(force=True)
    proc.expect(EOF)

    assert not (data_dir / 'pending-confirmation').exists()

    status = hawkbit.get_action_status()
    assert status[0]['type'] == 'finished'


def test_confirm_rollback(hawkbit, confirm_config, bundle_assigned,
                          rauc_dbus_install_success_scenario, preload_fake_reboot):
    """
    Full flow where the system boots back into the previous slot (rollback): reported as
    failure, state cleaned up.
    """
    config, data_dir = confirm_config
    # install target is rootfs.1 (primary), but the reboot lands back on the old slot
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
                         rauc_dbus_install_success_scenario, preload_fake_reboot):
    """
    Full flow where the reboot lands on the target slot but it is not yet marked good: no
    final feedback is sent, the pending state (target slot learned from GetPrimary) is
    retained, and the still-open deployment is not reinstalled.
    """
    config, data_dir = confirm_config
    rauc_dbus_install_success_scenario(reboot_to='rootfs.1', reboot_boot_status='bad')

    proc = run_pexpect(f'rauc-hawkbit-updater -c "{config}"')

    proc.expect('Software bundle installed, confirming after reboot')
    # booted into the target slot, but it is not yet confirmed good
    proc.expect('not yet confirmed')
    # the still-open offer must be ignored rather than reinstalled
    proc.expect('Ignoring deployment offer while an update confirmation is pending')

    # no final verdict must be reported
    assert proc.expect(['Update confirmed after reboot',
                        'Update rolled back to previous slot after reboot',
                        TIMEOUT], timeout=10) == 2

    proc.terminate(force=True)
    proc.expect(EOF)

    # the install side persisted the target slot learned from GetPrimary
    state = data_dir / 'pending-confirmation'
    assert state.exists()
    assert 'target_slot=rootfs.1' in state.read_text()

    status = hawkbit.get_action_status()
    assert status[0]['type'] != 'finished'
