/**
 * SPDX-License-Identifier: LGPL-2.1-only
 * SPDX-FileCopyrightText: 2021-2025 Bastian Krause <bst@pengutronix.de>, Pengutronix
 * SPDX-FileCopyrightText: 2018-2020 Lasse K. Mikkelsen <lkmi@prevas.dk>, Prevas A/S (www.prevas.com)
 *
 * @file
 * @brief RAUC client
 */

#include <gio/gio.h>
#include <glib-object.h>
#include <glib/gtypes.h>
#include <stdio.h>
#include "gobject/gclosure.h"
#include "rauc-installer.h"
#include "rauc-installer-gen.h"

static GThread *thread_install = NULL;

/**
 * @brief RAUC DBUS property changed callback
 *
 * @see https://github.com/rauc/rauc/blob/master/src/de.pengutronix.rauc.Installer.xml
 */
static void on_installer_status(GDBusProxy *proxy, GVariant *changed,
                                const gchar* const *invalidated, gpointer data)
{
        struct install_context *context = data;
        gint32 percentage;
        g_autofree gchar *message = NULL;

        g_return_if_fail(changed);
        g_return_if_fail(context);

        if (invalidated && invalidated[0]) {
                g_warning("RAUC DBUS service disappeared");
                g_mutex_lock(&context->status_mutex);
                context->status_result = 2;
                g_mutex_unlock(&context->status_mutex);
                g_main_loop_quit(context->mainloop);
                return;
        }

        if (context->notify_event) {
                gboolean status_received = FALSE;

                g_mutex_lock(&context->status_mutex);
                if (g_variant_lookup(changed, "Operation", "s", &message))
                        g_queue_push_tail(&context->status_messages, g_steal_pointer(&message));
                else if (g_variant_lookup(changed, "Progress", "(isi)", &percentage, &message,
                                          NULL))
                        g_queue_push_tail(&context->status_messages,
                                          g_strdup_printf("%3" G_GINT32_FORMAT "%% %s", percentage,
                                                          message));
                else if (g_variant_lookup(changed, "LastError", "s", &message) && message[0] != 0)
                        g_queue_push_tail(&context->status_messages,
                                          g_strdup_printf("LastError: %s", message));

                status_received = !g_queue_is_empty(&context->status_messages);
                g_mutex_unlock(&context->status_mutex);

                if (status_received)
                        g_main_context_invoke(context->loop_context, context->notify_event,
                                              context);
        }
}

/**
 * @brief RAUC DBUS complete signal callback
 *
 * @see https://github.com/rauc/rauc/blob/master/src/de.pengutronix.rauc.Installer.xml
 */
static void on_installer_completed(GDBusProxy *proxy, gint result, gpointer data)
{
        struct install_context *context = data;

        g_return_if_fail(context);

        g_mutex_lock(&context->status_mutex);
        context->status_result = result;
        g_mutex_unlock(&context->status_mutex);

        if (result >= 0)
                g_main_loop_quit(context->mainloop);
}

/**
 * @brief Create and init a install_context
 *
 * @return Pointer to initialized install_context struct. Should be freed by calling
 *         install_context_free().
 */
static struct install_context *install_context_new(void)
{
        struct install_context *context = g_new0(struct install_context, 1);

        g_mutex_init(&context->status_mutex);
        g_queue_init(&context->status_messages);
        context->status_result = -2;

        return context;
}

/**
 * @brief Free a install_context and its members
 *
 * @param[in] context the install_context struct that should be freed.
 *                    If NULL
 */
static void install_context_free(struct install_context *context)
{
        if (!context)
                return;

        g_free(context->bundle);
        g_free(context->auth_header);
        g_mutex_clear(&context->status_mutex);

        // make sure all pending events are processed
        while (g_main_context_iteration(context->loop_context, FALSE));
        g_main_context_unref(context->loop_context);

        g_assert_cmpint(context->status_result, >=, 0);
        g_assert_true(g_queue_is_empty(&context->status_messages));
        g_main_loop_unref(context->mainloop);
        g_free(context);
}

/**
 * @brief RAUC client mainloop
 *
 * Install mainloop running until installation completes.
 * @param[in] data pointer to a install_context struct.
 * @return NULL is always returned.
 */
static gpointer install_loop_thread(gpointer data)
{
        GBusType bus_type = (!g_strcmp0(g_getenv("DBUS_STARTER_BUS_TYPE"), "session"))
                            ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
        RInstaller *r_installer_proxy = NULL;
        g_autoptr(GError) error = NULL;
        g_auto(GVariantDict) args = G_VARIANT_DICT_INIT(NULL);
        struct install_context *context = NULL;

        g_return_val_if_fail(data, NULL);

        context = data;
        g_main_context_push_thread_default(context->loop_context);

        if (context->auth_header) {
                gchar *headers[2] = {NULL, NULL};
                headers[0] = context->auth_header;
                g_variant_dict_insert(&args, "http-headers", "^as", headers);
                g_variant_dict_insert(&args, "tls-no-verify", "b", !context->ssl_verify);
        }
        if (context->ssl_key && context->ssl_cert) {
                g_variant_dict_insert(&args, "tls-key", "s", context->ssl_key);
                g_variant_dict_insert(&args, "tls-cert", "s", context->ssl_cert);
                g_variant_dict_insert(&args, "tls-no-verify", "b", !context->ssl_verify);
        }

        g_debug("Creating RAUC DBUS proxy");
        r_installer_proxy = r_installer_proxy_new_for_bus_sync(
                bus_type, G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                "de.pengutronix.rauc", "/", NULL, &error);
        if (!r_installer_proxy) {
                g_warning("Failed to create RAUC DBUS proxy: %s", error->message);
                goto notify_complete;
        }
        if (g_signal_connect(r_installer_proxy, "g-properties-changed",
                             G_CALLBACK(on_installer_status), context) <= 0) {
                g_warning("Failed to connect properties-changed signal");
                goto out_loop;
        }
        if (g_signal_connect(r_installer_proxy, "completed",
                             G_CALLBACK(on_installer_completed), context) <= 0) {
                g_warning("Failed to connect completed signal");
                goto out_loop;
        }

        g_debug("Trying to contact RAUC DBUS service");
        if (!r_installer_call_install_bundle_sync(r_installer_proxy, context->bundle,
                                                  g_variant_dict_end(&args), NULL, &error)) {
                g_warning("%s", error->message);
                goto out_loop;
        }

        g_main_loop_run(context->mainloop);

out_loop:
        g_signal_handlers_disconnect_by_data(r_installer_proxy, context);

notify_complete:
        // Notify the result of the RAUC installation
        if (context->notify_complete)
                context->notify_complete(context);

        g_clear_pointer(&r_installer_proxy, g_object_unref);
        g_main_context_pop_thread_default(context->loop_context);

        // on wait, calling function will take care of freeing after reading context->status_result
        if (!context->keep_install_context)
                install_context_free(context);
        return NULL;
}

/**
 * @brief Create a synchronous RAUC D-Bus proxy.
 *
 * Properties are not loaded since only methods (GetPrimary, GetSlotStatus) are used.
 *
 * @param[out] error Error
 * @return new RInstaller proxy (unref with g_object_unref) or NULL (error set)
 */
static RInstaller *rauc_proxy_new(GError **error)
{
        GBusType bus_type = (!g_strcmp0(g_getenv("DBUS_STARTER_BUS_TYPE"), "session"))
                            ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;

        return r_installer_proxy_new_for_bus_sync(
                bus_type, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                "de.pengutronix.rauc", "/", NULL, error);
}

gboolean rauc_get_primary(gchar **primary, GError **error)
{
        RInstaller *proxy = NULL;
        gboolean res;

        g_return_val_if_fail(primary && *primary == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        proxy = rauc_proxy_new(error);
        if (!proxy)
                return FALSE;

        res = r_installer_call_get_primary_sync(proxy, primary, NULL, error);

        g_object_unref(proxy);
        return res;
}

/**
 * @brief Fetch RAUC's slot status array.
 *
 * @param[out] error Error
 * @return new GVariant of type a(sa{sv}) (array of (slotname, status-dict) tuples), to be
 *         unref'd by the caller, or NULL on failure (error set)
 */
static GVariant *rauc_get_slot_status(GError **error)
{
        RInstaller *proxy = NULL;
        GVariant *slot_status = NULL;

        g_return_val_if_fail(error == NULL || *error == NULL, NULL);

        proxy = rauc_proxy_new(error);
        if (!proxy)
                return NULL;

        r_installer_call_get_slot_status_sync(proxy, &slot_status, NULL, error);

        g_object_unref(proxy);
        return slot_status;
}

/**
 * @brief Copy selected fields out of a slot's status dict (a{sv}).
 *
 * @param[in]  slot_dict   a slot's status dict
 * @param[out] boot_status newly allocated boot-status if present, or NULL to ignore
 * @param[out] transaction newly allocated installed.transaction if present, or NULL
 */
static void slot_dict_extract(GVariant *slot_dict, gchar **boot_status, gchar **transaction)
{
        const gchar *bs = NULL, *tx = NULL;

        if (boot_status && g_variant_lookup(slot_dict, "boot-status", "&s", &bs))
                *boot_status = g_strdup(bs);
        if (transaction && g_variant_lookup(slot_dict, "installed.transaction", "&s", &tx))
                *transaction = g_strdup(tx);
}

gboolean rauc_get_booted_slot(gchar **booted_slot, gchar **boot_status,
                              gchar **transaction, GError **error)
{
        g_autoptr(GVariant) slot_status = NULL;
        GVariantIter iter;
        const gchar *slotname = NULL;
        GVariant *slot_dict = NULL;

        g_return_val_if_fail(booted_slot && *booted_slot == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        slot_status = rauc_get_slot_status(error);
        if (!slot_status)
                return FALSE;

        // the booted slot is the one RAUC marks with state "booted"
        g_variant_iter_init(&iter, slot_status);
        while (g_variant_iter_next(&iter, "(&s@a{sv})", &slotname, &slot_dict)) {
                const gchar *state = NULL;

                g_variant_lookup(slot_dict, "state", "&s", &state);
                if (!g_strcmp0(state, "booted")) {
                        *booted_slot = g_strdup(slotname);
                        slot_dict_extract(slot_dict, boot_status, transaction);
                        g_variant_unref(slot_dict);
                        return TRUE;
                }
                g_variant_unref(slot_dict);
        }

        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "RAUC reported no booted slot");
        return FALSE;
}

gboolean rauc_get_slot_transaction(const gchar *slot, gchar **transaction, GError **error)
{
        g_autoptr(GVariant) slot_status = NULL;
        GVariantIter iter;
        const gchar *slotname = NULL;
        GVariant *slot_dict = NULL;

        g_return_val_if_fail(slot, FALSE);
        g_return_val_if_fail(transaction && *transaction == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        slot_status = rauc_get_slot_status(error);
        if (!slot_status)
                return FALSE;

        g_variant_iter_init(&iter, slot_status);
        while (g_variant_iter_next(&iter, "(&s@a{sv})", &slotname, &slot_dict)) {
                if (!g_strcmp0(slotname, slot)) {
                        slot_dict_extract(slot_dict, NULL, transaction);
                        g_variant_unref(slot_dict);
                        return TRUE;
                }
                g_variant_unref(slot_dict);
        }

        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Slot '%s' not found in RAUC slot status", slot);
        return FALSE;
}

gboolean rauc_install(const gchar *bundle, const gchar *auth_header,
                      gchar *ssl_key, gchar *ssl_cert, gboolean ssl_verify,
                      GSourceFunc on_install_notify, GSourceFunc on_install_complete,
                      gboolean wait)
{
        GMainContext *loop_context = NULL;
        struct install_context *context = NULL;

        g_return_val_if_fail(bundle, FALSE);

        loop_context = g_main_context_new();
        context = install_context_new();
        context->bundle = g_strdup(bundle);
        context->auth_header = g_strdup(auth_header);
        context->ssl_key = ssl_key,
        context->ssl_cert = ssl_cert,
        context->ssl_verify = ssl_verify;
        context->notify_event = on_install_notify;
        context->notify_complete = on_install_complete;
        context->mainloop = g_main_loop_new(loop_context, FALSE);
        context->loop_context = loop_context;
        context->status_result = 2;
        context->keep_install_context = wait;

        // unref/free previous install thread by joining it
        if (thread_install)
                g_thread_join(thread_install);

        // start install thread
        thread_install = g_thread_new("installer", install_loop_thread, (gpointer) context);
        if (wait) {
                gboolean result;

                g_thread_join(thread_install);
                result = context->status_result == 0;

                install_context_free(context);
                return result;
        }

        // return immediately if we did not wait for the install thread
        return TRUE;
}
