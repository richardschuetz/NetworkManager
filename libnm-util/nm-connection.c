#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <string.h>
#include "nm-connection.h"
#include "nm-utils.h"

typedef struct {
	GHashTable *settings;
} NMConnectionPrivate;

#define NM_CONNECTION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_CONNECTION, NMConnectionPrivate))

G_DEFINE_TYPE (NMConnection, nm_connection, G_TYPE_OBJECT)

enum {
	SECRETS_UPDATED,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static GHashTable *registered_setting_creators = NULL;

static void
register_default_creators (void)
{
	int i;
	const struct {
		const char *name;
		NMSettingCreateFn fn;
	} default_map[] = {
		{ NM_SETTING_CONNECTION,        nm_setting_connection_new_from_hash      },
		{ NM_SETTING_WIRED,             nm_setting_wired_new_from_hash     },
		{ NM_SETTING_WIRELESS,          nm_setting_wireless_new_from_hash  },
		{ NM_SETTING_IP4_CONFIG,        nm_setting_ip4_config_new_from_hash },
		{ NM_SETTING_WIRELESS_SECURITY, nm_setting_wireless_security_new_from_hash  },
		{ NM_SETTING_PPP,               nm_setting_ppp_new_from_hash       },
		{ NM_SETTING_VPN,               nm_setting_vpn_new_from_hash },
		{ NM_SETTING_VPN_PROPERTIES,    nm_setting_vpn_properties_new_from_hash },
		{ NULL, NULL}
	};

	for (i = 0; default_map[i].name; i++)
		nm_setting_parser_register (default_map[i].name, default_map[i].fn);
}

void
nm_setting_parser_register (const char *name, NMSettingCreateFn creator)
{
	g_return_if_fail (name != NULL);
	g_return_if_fail (creator != NULL);
	
	if (!registered_setting_creators)
		registered_setting_creators = g_hash_table_new_full (g_str_hash, g_str_equal,
															 (GDestroyNotify) g_free, NULL);

	if (g_hash_table_lookup (registered_setting_creators, name))
		g_warning ("Already have a creator function for '%s', overriding", name);

	g_hash_table_insert (registered_setting_creators, g_strdup (name), creator);
}

void
nm_setting_parser_unregister (const char *name)
{
	if (registered_setting_creators)
		g_hash_table_remove (registered_setting_creators, name);
}

static void
parse_one_setting (gpointer key, gpointer value, gpointer user_data)
{
	NMConnection *connection = (NMConnection *) user_data;
	NMSettingCreateFn fn;
	NMSetting *setting;

	fn = (NMSettingCreateFn) g_hash_table_lookup (registered_setting_creators, key);
	if (fn) {
		setting = fn ((GHashTable *) value);
		if (setting)
			nm_connection_add_setting (connection, setting);
	} else
		g_warning ("Unknown setting '%s'", (char *) key);
}

void
nm_connection_add_setting (NMConnection *connection, NMSetting *setting)
{
	NMConnectionPrivate *priv;

	g_return_if_fail (NM_IS_CONNECTION (connection));
	g_return_if_fail (setting != NULL);

	priv = NM_CONNECTION_GET_PRIVATE (connection);
	g_hash_table_insert (priv->settings, setting->name, setting);
}

NMSetting *
nm_connection_get_setting (NMConnection *connection, const char *setting_name)
{
	NMConnectionPrivate *priv;

	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);
	g_return_val_if_fail (setting_name != NULL, NULL);

	priv = NM_CONNECTION_GET_PRIVATE (connection);
	return (NMSetting *) g_hash_table_lookup (priv->settings, setting_name);
}

gboolean
nm_connection_compare (NMConnection *connection, NMConnection *other)
{
	if (!connection && !other)
		return TRUE;

	if (!connection || !other)
		return FALSE;

	/* FIXME: Implement */

	return FALSE;
}

void
nm_connection_update_secrets (NMConnection *connection,
                              const char *setting_name,
                              GHashTable *secrets)
{
	NMSetting *setting;

	g_return_if_fail (NM_IS_CONNECTION (connection));
	g_return_if_fail (setting_name != NULL);
	g_return_if_fail (secrets != NULL);

	setting = nm_connection_get_setting (connection, setting_name);
	if (!setting) {
		g_warning ("Unhandled settings object for secrets update.");
		return;
	}

	if (!nm_setting_update_secrets (setting, secrets)) {
		g_warning ("Error updating secrets for setting '%s'", setting_name);
		return;
	}

	g_signal_emit (connection, signals[SECRETS_UPDATED], 0, setting_name);
}

typedef struct NeedSecretsInfo {
	GPtrArray * secrets;
	char * setting_name;
} NeedSecretsInfo;

static void
need_secrets_check (gpointer key, gpointer data, gpointer user_data)
{
	NMSetting *setting = (NMSetting *) data;
	NeedSecretsInfo * info = (NeedSecretsInfo *) user_data;

	// FIXME: allow more than one setting to say it needs secrets
	if (info->secrets)
		return;

	info->secrets = nm_setting_need_secrets (setting);
	if (info->secrets)
		info->setting_name = key;
}

const char *
nm_connection_need_secrets (NMConnection *connection)
{
	NMConnectionPrivate *priv;
	NeedSecretsInfo info = { NULL, NULL };

	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);

	priv = NM_CONNECTION_GET_PRIVATE (connection);
	g_hash_table_foreach (priv->settings, need_secrets_check, &info);

	// FIXME: do something with requested secrets rather than asking for
	// all of them.  Maybe make info.secrets a hash table mapping
	// settings name :: [list of secrets key names].
	if (info.secrets) {
		g_ptr_array_free (info.secrets, TRUE);
		return info.setting_name;
	}

	return NULL;
}

static void
clear_setting_secrets (gpointer key, gpointer data, gpointer user_data)
{
	NMSetting *setting = (NMSetting *) data;

	nm_setting_clear_secrets (setting);
}

void
nm_connection_clear_secrets (NMConnection *connection)
{
	NMConnectionPrivate *priv;

	g_return_if_fail (NM_IS_CONNECTION (connection));

	priv = NM_CONNECTION_GET_PRIVATE (connection);
	g_hash_table_foreach (priv->settings, clear_setting_secrets, NULL);
}

static void
add_one_setting_to_hash (gpointer key, gpointer data, gpointer user_data)
{
	NMSetting *setting = (NMSetting *) data;
	GHashTable *connection_hash = (GHashTable *) user_data;
	GHashTable *setting_hash;

	g_return_if_fail (setting != NULL);
	g_return_if_fail (connection_hash != NULL);

	setting_hash = nm_setting_to_hash (setting);
	if (setting_hash)
		g_hash_table_insert (connection_hash,
							 g_strdup (setting->name),
							 setting_hash);
}

GHashTable *
nm_connection_to_hash (NMConnection *connection)
{
	NMConnectionPrivate *priv;
	GHashTable *connection_hash;

	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);

	connection_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
											 (GDestroyNotify) g_free,
											 (GDestroyNotify) g_hash_table_destroy);

	priv = NM_CONNECTION_GET_PRIVATE (connection);
	g_hash_table_foreach (priv->settings, add_one_setting_to_hash, connection_hash);

	/* Don't send empty hashes */
	if (g_hash_table_size (connection_hash) < 1) {
		g_hash_table_destroy (connection_hash);
		connection_hash = NULL;
	}

	return connection_hash;
}

typedef struct ForEachValueInfo {
	NMSettingValueIterFn func;
	gpointer user_data;
} ForEachValueInfo;

static void
for_each_setting (gpointer key, gpointer value, gpointer user_data)
{
	ForEachValueInfo *info = (ForEachValueInfo *) user_data;
	NMSetting *setting = (NMSetting *) value;

	nm_setting_enumerate_values (setting, info->func, info->user_data);
}

void
nm_connection_for_each_setting_value (NMConnection *connection,
                                       NMSettingValueIterFn func,
                                       gpointer user_data)
{
	NMConnectionPrivate *priv;
	ForEachValueInfo *info;

	g_return_if_fail (NM_IS_CONNECTION (connection));
	g_return_if_fail (func != NULL);

	priv = NM_CONNECTION_GET_PRIVATE (connection);

	info = g_slice_new0 (ForEachValueInfo);
	if (!info) {
		g_warning ("Not enough memory to enumerate values.");
		return;
	}
	info->func = func;
	info->user_data = user_data;

	g_hash_table_foreach (priv->settings, for_each_setting, info);

	g_slice_free (ForEachValueInfo, info);
}

static char *
gvalue_to_string (GValue *val)
{
	char *ret;
	GType type;
	GString *str;
	gboolean need_comma = FALSE;

	type = G_VALUE_TYPE (val);
	switch (type) {
	case G_TYPE_STRING:
		ret = g_strdup (g_value_get_string (val));
		break;
	case G_TYPE_INT:
		ret = g_strdup_printf ("%d", g_value_get_int (val));
		break;
	case G_TYPE_UINT:
		ret = g_strdup_printf ("%u", g_value_get_uint (val));
		break;
	case G_TYPE_BOOLEAN:
		ret = g_strdup_printf ("%s", g_value_get_boolean (val) ? "True" : "False");
		break;
	case G_TYPE_UCHAR:
		ret = g_strdup_printf ("%d", g_value_get_uchar (val));
		break;

	default:
		/* These return dynamic values and thus can't be 'case's */
		if (type == DBUS_TYPE_G_UCHAR_ARRAY)
			ret = nm_utils_garray_to_string ((GArray *) g_value_get_boxed (val));
		else if (type == dbus_g_type_get_collection ("GSList", G_TYPE_STRING)) {
			GSList *iter;

			str = g_string_new ("[");
			for (iter = g_value_get_boxed (val); iter; iter = iter->next) {
				if (need_comma)
					g_string_append (str, ", ");
				else
					need_comma = TRUE;

				g_string_append (str, (char *) iter->data);
			}
			g_string_append (str, "]");

			ret = g_string_free (str, FALSE);
		} else if (type == dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_UCHAR_ARRAY)) {
			/* Array of arrays of chars, like wireless seen-bssids for example */
			int i;
			GPtrArray *ptr_array;

			str = g_string_new ("[");

			ptr_array = (GPtrArray *) g_value_get_boxed (val);
			for (i = 0; i < ptr_array->len; i++) {
				ret = nm_utils_garray_to_string ((GArray *) g_ptr_array_index (ptr_array, i));

				if (need_comma)
					g_string_append (str, ", ");
				else
					need_comma = TRUE;

				g_string_append (str, ret);
				g_free (ret);
			}

			g_string_append (str, "]");
			ret = g_string_free (str, FALSE);
		} else
			ret = g_strdup_printf ("Value with type %s", g_type_name (type));
	}

	return ret;
}

static void
dump_setting_member (gpointer key, gpointer value, gpointer user_data)
{
	char *val_as_str;

	val_as_str = gvalue_to_string ((GValue *) value);
	g_message ("\t%s : '%s'", (char *) key, val_as_str ? val_as_str : "(null)");
	g_free (val_as_str);
}

static void
dump_setting (gpointer key, gpointer value, gpointer user_data)
{
	g_message ("Setting '%s'", (char *) key);
	g_hash_table_foreach ((GHashTable *) value, dump_setting_member, NULL);
	g_message ("-------------------");
}

void
nm_connection_dump (NMConnection *connection)
{
	GHashTable *hash;

	g_return_if_fail (NM_IS_CONNECTION (connection));

	/* Convert the connection to hash so that we can introspect it */
	hash = nm_connection_to_hash (connection);
	g_hash_table_foreach (hash, dump_setting, NULL);
	g_hash_table_destroy (hash);
}

NMConnection *
nm_connection_new (void)
{
	GObject *object;

	if (!registered_setting_creators)
		register_default_creators ();

	object = g_object_new (NM_TYPE_CONNECTION, NULL);

	return NM_CONNECTION (object);
}

NMConnection *
nm_connection_new_from_hash (GHashTable *hash)
{
	NMConnection *connection;
	NMConnectionPrivate *priv;

	g_return_val_if_fail (hash != NULL, NULL);

	connection = nm_connection_new ();
	g_hash_table_foreach (hash, parse_one_setting, connection);

	priv = NM_CONNECTION_GET_PRIVATE (connection);

	if (g_hash_table_size (priv->settings) < 1) {
		g_warning ("No settings found.");
		g_object_unref (connection);
		return NULL;
	}

	if (!nm_settings_verify (priv->settings)) {
		g_object_unref (connection);
		return NULL;
	}

	return connection;
}

static void
nm_connection_init (NMConnection *connection)
{
	NMConnectionPrivate *priv = NM_CONNECTION_GET_PRIVATE (connection);

	priv->settings = g_hash_table_new (g_str_hash, g_str_equal);
}

static void
finalize (GObject *object)
{
	NMConnection *connection = NM_CONNECTION (object);
	NMConnectionPrivate *priv = NM_CONNECTION_GET_PRIVATE (connection);

	g_hash_table_destroy (priv->settings);
	priv->settings = NULL;

	G_OBJECT_CLASS (nm_connection_parent_class)->finalize (object);
}

static void
nm_connection_class_init (NMConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMConnectionPrivate));

	/* virtual methods */
	object_class->finalize = finalize;

	/* Signals */
	signals[SECRETS_UPDATED] =
		g_signal_new ("secrets-updated",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMConnectionClass, secrets_updated),
					  NULL, NULL,
					  g_cclosure_marshal_VOID__STRING,
					  G_TYPE_NONE, 1,
					  G_TYPE_STRING);
}

