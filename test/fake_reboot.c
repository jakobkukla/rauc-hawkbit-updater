/* SPDX-License-Identifier: LGPL-2.1-only */

/*
 * LD_PRELOAD shim used by the confirm_after_reboot tests: it turns reboot() into a no-op
 * so the tests can exercise the real install -> proceeding -> reboot -> confirm flow in a
 * single process without the host actually rebooting. Not linked into the updater.
 */
int reboot(int howto)
{
        (void) howto;
        return 0;
}
