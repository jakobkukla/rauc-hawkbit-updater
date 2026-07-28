/**
 * SPDX-License-Identifier: LGPL-2.1-only
 * SPDX-FileCopyrightText: 2021-2025 Bastian Krause <bst@pengutronix.de>, Pengutronix
 * SPDX-FileCopyrightText: 2018-2020 Lasse K. Mikkelsen <lkmi@prevas.dk>, Prevas A/S (www.prevas.com)
 *
 * @file
 * @brief RAUC HawkBit updater daemon
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include "rauc-installer.h"
#include "hawkbit-client.h"
#include "config-file.h"
#include "log.h"

#define PROGRAM "rauc-hawkbit-updater"
#define VERSION PROJECT_VERSION

// program arguments
static gchar *config_file          = NULL;
static gboolean opt_version        = FALSE;
static gboolean opt_debug          = FALSE;
static gboolean opt_run_once       = FALSE;
static gboolean opt_output_systemd = FALSE;

// Commandline options
static GOptionEntry entries[] =
{
        { "config-file",      'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &config_file,           "Configuration file",                       NULL },
        { "version",          'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,     &opt_version,           "Version information",                      NULL },
        { "debug",            'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,     &opt_debug,             "Enable debug output",                      NULL },
        { "run-once",         'r', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,     &opt_run_once,          "Check and install new software and exit",  NULL },
#ifdef WITH_SYSTEMD
        { "output-systemd",   's', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,     &opt_output_systemd,    "Enable output to systemd",                 NULL },
#endif
        { NULL }
};

// hawkbit callbacks
static GSourceFunc notify_hawkbit_install_progress;
static GSourceFunc notify_hawkbit_install_complete;


/**
 * @brief GSourceFunc callback for install thread, consumes RAUC progress messages, logs them and
 * passes them on to notify_hawkbit_install_progress().
 *
 * @param[in] data install_context pointer allowing access to received status messages
 * @return G_SOURCE_REMOVE is always returned
 */
static gboolean on_rauc_install_progress_cb(gpointer data)
{
        struct install_context *context = data;

        g_return_val_if_fail(data, G_SOURCE_REMOVE);

        g_mutex_lock(&context->status_mutex);
        while (!g_queue_is_empty(&context->status_messages)) {
                g_autofree gchar *msg = g_queue_pop_head(&context->status_messages);
                g_message("Installing: %s : %s", context->bundle, msg);
                // notify hawkbit server about progress
                notify_hawkbit_install_progress(msg);
        }
        g_mutex_unlock(&context->status_mutex);

        return G_SOURCE_REMOVE;
}

/**
 * @brief GSourceFunc callback for install thread, consumes RAUC installation status result
 *        (on complete) and passes it on to notify_hawkbit_install_complete().
 *
 * @param[in] data install_context pointer allowing access to received status result
 * @return G_SOURCE_REMOVE is always returned
 */
static gboolean on_rauc_install_complete_cb(gpointer data)
{
        struct install_context *context = data;
        struct on_install_complete_userdata userdata;

        g_return_val_if_fail(data, G_SOURCE_REMOVE);

        userdata.install_success = (context->status_result == 0);

        // notify hawkbit about install result
        notify_hawkbit_install_complete(&userdata);

        return G_SOURCE_REMOVE;
}

/**
 * @brief GSourceFunc callback for download thread, or main thread in case of HTTP streaming
 *        installation. Triggers RAUC installation.
 *
 * @param[in] data on_new_software_userdata pointer
 * @return G_SOURCE_REMOVE is always returned
 */
static gboolean on_new_software_ready_cb(gpointer data)
{
        struct on_new_software_userdata *userdata = data;

        g_return_val_if_fail(data, G_SOURCE_REMOVE);

        notify_hawkbit_install_progress = userdata->install_progress_callback;
        notify_hawkbit_install_complete = userdata->install_complete_callback;
        userdata->install_success = rauc_install(userdata->file, userdata->auth_header,
                                                 userdata->ssl_key, userdata->ssl_cert,
                                                 userdata->ssl_verify,
                                                 on_rauc_install_progress_cb,
                                                 on_rauc_install_complete_cb, run_once);

        return G_SOURCE_REMOVE;
}

/**
 * @brief Validate the runtime prerequisites of the confirm_after_reboot feature.
 *
 * @param[in]  config Config with confirm_after_reboot enabled
 * @param[out] error  Error
 * @return TRUE if all prerequisites are met, FALSE otherwise (error set)
 */
static gboolean check_confirm_after_reboot_requirements(Config *config, GError **error)
{
        g_autofree gchar *booted_slot = NULL;

        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        // The confirmation is driven by the poll loop across a reboot; run-once mode exits
        // after a single poll and cannot re-evaluate the boot verdict.
        if (run_once) {
                g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                            "'confirm_after_reboot' cannot be combined with run-once mode (-r)");
                return FALSE;
        }

        if (!g_file_test(config->data_directory, G_FILE_TEST_IS_DIR)) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOTDIR,
                            "data_directory '%s' does not exist or is not a directory",
                            config->data_directory);
                return FALSE;
        }
        if (g_access(config->data_directory, W_OK) != 0) {
                g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                            "data_directory '%s' is not writable: %s",
                            config->data_directory, g_strerror(errno));
                return FALSE;
        }

        // Verify RAUC's D-Bus interface is reachable and exposes the booted slot / slot
        // status this feature relies on.
        if (!rauc_get_booted_slot(&booted_slot, NULL, NULL, error)) {
                g_prefix_error(error,
                               "RAUC slot status unavailable (required for confirm_after_reboot): ");
                return FALSE;
        }

        return TRUE;
}

int main(int argc, char **argv)
{
        g_autoptr(GError) error = NULL;
        g_autoptr(GOptionContext) context = NULL;
        g_auto(GStrv) args = NULL;
        GLogLevelFlags log_level;
        g_autoptr(Config) config = NULL;
        GLogLevelFlags fatal_mask;

        fatal_mask = g_log_set_always_fatal(G_LOG_FATAL_MASK);
        fatal_mask |= G_LOG_LEVEL_CRITICAL;
        g_log_set_always_fatal(fatal_mask);

        args = g_strdupv(argv);

        context = g_option_context_new("");
        g_option_context_add_main_entries(context, entries, NULL);
        if (!g_option_context_parse_strv(context, &args, &error)) {
                g_printerr("option parsing failed: %s\n", error->message);
                return 1;
        }

        if (opt_version) {
                g_printf("Version %s\n", PROJECT_VERSION);
                return 0;
        }

        if (!config_file) {
                g_printerr("No configuration file given\n");
                return 2;
        }

        if (!g_file_test(config_file, G_FILE_TEST_EXISTS)) {
                g_printerr("No such configuration file: %s\n", config_file);
                return 3;
        }

        run_once = opt_run_once;

        config = load_config_file(config_file, &error);
        if (!config) {
                g_printerr("Loading config file failed: %s\n", error->message);
                return 4;
        }

        log_level = (opt_debug) ? G_LOG_LEVEL_MASK : config->log_level;

        setup_logging(PROGRAM, log_level, opt_output_systemd);

        if (config->confirm_after_reboot &&
            !check_confirm_after_reboot_requirements(config, &error)) {
                g_printerr("%s\n", error->message);
                return 5;
        }

        hawkbit_init(config, on_new_software_ready_cb);

        return hawkbit_start_service_sync();
}
