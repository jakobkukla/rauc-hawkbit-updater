/**
 * SPDX-License-Identifier: LGPL-2.1-only
 * SPDX-FileCopyrightText: 2018-2020 Lasse K. Mikkelsen <lkmi@prevas.dk>, Prevas A/S (www.prevas.com)
 */

#ifndef __RAUC_INSTALLER_H__
#define __RAUC_INSTALLER_H__

#include <glib.h>

/**
 * @brief struct that contains the context of an Rauc installation.
 */
struct install_context {
        gchar *bundle;                /**< Rauc bundle file to install */
        gchar *auth_header;           /**< Authentication header for bundle streaming */
        gchar *ssl_key;               /**< SSL client authentication key */
        gchar *ssl_cert;              /**< SSL client authentication certificate */
        gboolean ssl_verify;          /**< Whether to ignore server cert verification errors */
        GSourceFunc notify_event;     /**< Callback function */
        GSourceFunc notify_complete;  /**< Callback function */
        GMutex status_mutex;          /**< Mutex used for accessing status_messages */
        GQueue status_messages;       /**< Queue of status messages from Rauc DBUS */
        gint status_result;           /**< The result of the installation */
        GMainLoop *mainloop;          /**< The installation GMainLoop  */
        GMainContext *loop_context;   /**< GMainContext for the GMainLoop */
        gboolean keep_install_context; /**< Whether the installation thread should free this struct or keep it */
};

/**
 * @brief RAUC install bundle
 *
 * @param[in] bundle RAUC bundle file (.raucb) to install.
 * @param[in] auth_header Authentication header on HTTP streaming installation or NULL on normal
 *                        installation.
 * @param[in] ssl_key Client authentication key or NULL on normal installation.
 * @param[in] ssl_cert Client authentication certificate or NULL on normal installation.
 * @param[in] ssl_verify Whether to ignore server cert verification errors.
 * @param[in] on_install_notify Callback function to be called with status info during
 *                              installation.
 * @param[in] on_install_complete Callback function to be called with the result of the
 *                                installation.
 * @param[in] wait Whether to wait until install thread finished or not.
 * @return for wait=TRUE, TRUE if installation succeeded, FALSE otherwise; for
 *         wait=FALSE TRUE is always returned immediately
 */
gboolean rauc_install(const gchar *bundle, const gchar *auth_header,
                gchar *ssl_key, gchar *ssl_cert, gboolean ssl_verify,
                GSourceFunc on_install_notify, GSourceFunc on_install_complete, gboolean wait);

/**
 * @brief Query RAUC for the primary (next-boot) slot.
 *
 * @param[out] primary Newly allocated primary slot name on success
 * @param[out] error   Error
 * @return TRUE on success, FALSE otherwise (error set)
 */
gboolean rauc_get_primary(gchar **primary, GError **error);

/**
 * @brief Query RAUC for the slot the system booted from and its status.
 *
 * The booted slot is the one RAUC marks with state "booted" in its slot status; it is
 * returned as the RAUC slot name, directly comparable to rauc_get_primary()'s output.
 *
 * @param[out] booted_slot Newly allocated booted slot name on success
 * @param[out] boot_status Newly allocated boot status ("good"/"bad"/...) or NULL, or NULL
 *                         to ignore
 * @param[out] transaction Newly allocated installed.transaction or NULL, or NULL to ignore
 * @param[out] error       Error
 * @return TRUE on success, FALSE otherwise (error set)
 */
gboolean rauc_get_booted_slot(gchar **booted_slot, gchar **boot_status,
                gchar **transaction, GError **error);

/**
 * @brief Query the installed.transaction of a specific RAUC slot.
 *
 * @param[in]  slot        slot name to query (e.g. "rootfs.1")
 * @param[out] transaction Newly allocated installed.transaction, or set to NULL if the
 *                         slot has no recorded transaction
 * @param[out] error       Error
 * @return TRUE if the slot was found, FALSE otherwise (error set)
 */
gboolean rauc_get_slot_transaction(const gchar *slot, gchar **transaction, GError **error);

#endif // __RAUC_INSTALLER_H__
