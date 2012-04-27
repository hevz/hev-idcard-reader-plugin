/*
 ============================================================================
 Name        : hev-plugin.c
 Author      : Heiher <admin@heiher.info>
 Version     : 0.0.1
 Copyright   : Copyright (C) 2012 everyone.
 Description : 
 ============================================================================
 */

#define XP_UNIX		1

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <nspr.h>
#include <npapi.h>
#include <npruntime.h>
#include <npfunctions.h>
#include <gio/gio.h>

#include "hev-plugin.h"

#define PLUGIN_NAME         "HevIDCardReader"
#define PLUGIN_MIME_TYPES   "application/x-hevidcardreader"
#define PLUGIN_DESCRIPTION  "ID Card Reader plugin."

typedef struct _HevPluginPrivate HevPluginPrivate;

struct _HevPluginPrivate
{
	NPObject *obj;
	GDBusConnection *dbus_conn;
	GDBusProxy *dbus_proxy_manager;
	GList *dbus_proxy_readers;
	gboolean reader_status;
	gboolean card_status;
	GVariant *card_info;
	NPObject *reader_status_notify_handler;
	NPObject *card_status_notify_handler;
	NPObject *card_info_notify_handler;
};

typedef struct _HevScriptableObj HevScriptableObj;

struct _HevScriptableObj
{
	NPObject obj;
	NPP npp;
};

static NPNetscapeFuncs *netscape_funcs = NULL;
static NPClass npklass = { 0 };
static NPIdentifier npi_get_reader_status = { 0 };
static NPIdentifier npi_get_card_status = { 0 };
static NPIdentifier npi_get_card_info = { 0 };
static NPIdentifier npi_set_reader_status_notify_handler = { 0 };
static NPIdentifier npi_set_card_status_notify_handler = { 0 };
static NPIdentifier npi_set_card_info_notify_handler = { 0 };

static NPObject * NPO_Allocate(NPP npp, NPClass *klass);
static void NPO_Deallocate(NPObject *npobj);
static bool NPO_HasMethod(NPObject *npobj, NPIdentifier method_name);
static bool NPO_Invoke(NPObject *npobj, NPIdentifier name,
			const NPVariant *args, uint32_t arg_count,
			NPVariant *result);

static void g_debug_log_null_handler(const gchar *domain,
			GLogLevelFlags level, const gchar *message,
			gpointer user_data);

static void g_bus_get_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);
static void g_dbus_proxy_manager_new_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);
static void g_dbus_proxy_manager_enumerate_devices_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);
static void g_dbus_proxy_reader_new_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);
static void g_dbus_proxy_reader_get_status_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);
static void g_dbus_proxy_reader_get_card_status_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);
static void g_dbus_proxy_reader_get_card_info_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data);

static void g_dbus_proxy_manager_g_signal_handler(GDBusProxy *proxy,
			gchar *sender_name, gchar *signal_name, GVariant *params,
			gpointer user_data);
static void g_dbus_proxy_reader_g_signal_handler(GDBusProxy *proxy,
			gchar *sender_name, gchar *signal_name, GVariant *params,
			gpointer user_data);

static void hev_plugin_add_reader(NPP instance, const gchar *object_path);
static void hev_plugin_remove_reader(NPP instance, const gchar *object_path);

NP_EXPORT(NPError) NP_Initialize(NPNetscapeFuncs *npn_funcs, NPPluginFuncs *npp_funcs)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	/* Check errors */
	if((NULL==npn_funcs) || (NULL==npp_funcs))
	  return NPERR_INVALID_FUNCTABLE_ERROR;

	/* Check version and struct size */
	if(NP_VERSION_MAJOR < (npn_funcs->version>>8))
	  return NPERR_INCOMPATIBLE_VERSION_ERROR;
	if(sizeof(NPNetscapeFuncs) > npn_funcs->size)
	  return NPERR_INVALID_FUNCTABLE_ERROR;
	if(sizeof(NPPluginFuncs) > npp_funcs->size)
	  return NPERR_INVALID_FUNCTABLE_ERROR;

	/* Save netscape functions */
	netscape_funcs = npn_funcs;

	/* Overwrite plugin functions */
	npp_funcs->version = (NP_VERSION_MAJOR<<8) + NP_VERSION_MINOR;
	npp_funcs->size = sizeof(NPPluginFuncs);
	npp_funcs->newp = NPP_New;
	npp_funcs->destroy = NPP_Destroy;
	npp_funcs->setwindow = NPP_SetWindow;
	npp_funcs->newstream = NPP_NewStream;
	npp_funcs->destroystream = NPP_DestroyStream;
	npp_funcs->writeready = NPP_WriteReady;
	npp_funcs->write= NPP_Write;
	npp_funcs->asfile = NPP_StreamAsFile;
	npp_funcs->print = NPP_Print;
	npp_funcs->urlnotify = NPP_URLNotify;
	npp_funcs->event = NPP_HandleEvent;
	npp_funcs->getvalue = NPP_GetValue;

	/* NP Object Class */
	npklass.structVersion = NP_CLASS_STRUCT_VERSION;
	npklass.allocate = NPO_Allocate;
	npklass.deallocate = NPO_Deallocate;
	npklass.hasMethod = NPO_HasMethod;
	npklass.invoke = NPO_Invoke;

	/* NP Object method property identifier */
	npi_get_reader_status = NPN_GetStringIdentifier("GetReaderStatus");
	npi_get_card_status = NPN_GetStringIdentifier("GetCardStatus");
	npi_get_card_info = NPN_GetStringIdentifier("GetCardInfo");
	npi_set_reader_status_notify_handler = NPN_GetStringIdentifier("SetReaderStatusNotifyHandler");
	npi_set_card_status_notify_handler = NPN_GetStringIdentifier("SetCardStatusNotifyHandler");
	npi_set_card_info_notify_handler = NPN_GetStringIdentifier("SetCardInfoNotifyHandler");

	/* GTK+ Initialize */
	gtk_init(0, 0);

	return NPERR_NO_ERROR;
}

NP_EXPORT(NPError) NP_Shutdown(void)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return NPERR_NO_ERROR;
}

NP_EXPORT(const char *) NP_GetMIMEDescription(void)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return PLUGIN_MIME_TYPES":"PLUGIN_NAME":"PLUGIN_DESCRIPTION;
}

NP_EXPORT(NPError) NP_GetValue(void *future, NPPVariable variable, void *value)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return NPP_GetValue(future, variable, value);
}

NPError NPN_SetValue(NPP instance, NPPVariable variable, void *value)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->setvalue(instance, variable, value);
}

NPError NPN_GetValue(NPP instance, NPNVariable variable, void *value)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->getvalue(instance, variable, value);
}

NPError NPN_DestroyStream(NPP instance, NPStream *stream,
			NPReason reason)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->destroystream(instance, stream, reason);
}

void * NPN_MemAlloc(uint32_t size)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->memalloc(size);
}

void NPN_MemFree(void *ptr)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	netscape_funcs->memfree(ptr);
}

NPIdentifier NPN_GetStringIdentifier(const NPUTF8 *name)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->getstringidentifier(name);
}

NPUTF8 *NPN_UTF8FromIdentifier(NPIdentifier identifier)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->utf8fromidentifier(identifier);
}

NPObject *NPN_CreateObject(NPP instance, NPClass *klass)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->createobject(instance, klass);
}

NPObject *NPN_RetainObject(NPObject *npobj)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->retainobject(npobj);
}

void NPN_ReleaseObject(NPObject *npobj)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->releaseobject(npobj);
}

bool NPN_InvokeDefault(NPP npp, NPObject *npobj, const NPVariant *args,
                       uint32_t arg_count, NPVariant *result)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->invokeDefault(npp, npobj,
				args, arg_count, result);
}

bool NPN_GetProperty(NPP npp, NPObject *npobj, NPIdentifier name,
			NPVariant *result)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return netscape_funcs->getproperty(npp, npobj, name, result);
}

void NPN_ReleaseVariantValue(NPVariant *variant)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	netscape_funcs->releasevariantvalue(variant);
}

NPError NPP_New(NPMIMEType plugin_type, NPP instance,
			uint16_t mode, int16_t argc, char* argn[], char* argv[],
			NPSavedData *saved)
{
	HevPluginPrivate *priv = NULL;
	NPError err = NPERR_NO_ERROR;
	PRBool xembed = PR_FALSE;
	NPNToolkitType toolkit = 0;
	uint16_t i = 0;
	gboolean debug = FALSE;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

    err = NPN_GetValue(instance, NPNVSupportsXEmbedBool, &xembed);
    if((NPERR_NO_ERROR!=err) || (PR_TRUE!=xembed))
    {
		g_debug("%s:%d[%s]=>(%s)", __FILE__, __LINE__,
					__FUNCTION__, "XEmbed nonsupport!");
        return NPERR_GENERIC_ERROR;
    }

    err = NPN_GetValue(instance, NPNVToolkit, &toolkit);
    if((NPERR_NO_ERROR!=err) || (NPNVGtk2!=toolkit))
    {
		g_debug("%s:%d[%s]=>(%s)", __FILE__, __LINE__,
					__FUNCTION__, "GTK+ Toolkit isn't 2!");
        return NPERR_GENERIC_ERROR;
    }

	priv = NPN_MemAlloc(sizeof(HevPluginPrivate));
	if(NULL == priv)
	{
		g_debug("%s:%d[%s]=>(%s)", __FILE__, __LINE__,
					__FUNCTION__, "Alloc private data failed!");
		return NPERR_OUT_OF_MEMORY_ERROR;
	}
	memset(priv, 0, sizeof(HevPluginPrivate));
	instance->pdata = priv;

	/* Params */
	for(i=0; i<argc; i++)
	{
		if((0==g_strcmp0(argn[i], "debug")) &&
					(0==g_ascii_strcasecmp(argv[i], "true")))
		  debug = TRUE;
	}

	/* Reset debug log handler */
	if(FALSE == debug)
	  g_log_set_handler(NULL, G_LOG_LEVEL_DEBUG,
				  g_debug_log_null_handler, NULL);

	/* Reader status */
	priv->reader_status = FALSE;
	/* Card status */
	priv->card_status = FALSE;
	/* Card info */
	priv->card_info = NULL;
	/* Reader status notify handler */
	priv->reader_status_notify_handler = NULL;
	/* Card status notify handler */
	priv->card_status_notify_handler = NULL;
	/* Card info notify handler */
	priv->card_info_notify_handler = NULL;

	/* DBus connection */
	g_bus_get(G_BUS_TYPE_SYSTEM, NULL,
				g_bus_get_handler, instance);

	return NPERR_NO_ERROR;
}

NPError NPP_Destroy(NPP instance, NPSavedData **saved)
{
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	if(priv->card_info)
	  g_variant_unref(priv->card_info);

	if(priv->reader_status_notify_handler)
	  NPN_ReleaseObject(priv->reader_status_notify_handler);
	if(priv->card_status_notify_handler)
	  NPN_ReleaseObject(priv->card_status_notify_handler);
	if(priv->card_info_notify_handler)
	  NPN_ReleaseObject(priv->card_info_notify_handler);

	g_list_free_full(priv->dbus_proxy_readers, g_object_unref);
	g_object_unref(G_OBJECT(priv->dbus_proxy_manager));
	g_object_unref(G_OBJECT(priv->dbus_conn));
	NPN_MemFree(instance->pdata);

	return NPERR_NO_ERROR;
}

NPError NPP_SetWindow(NPP instance, NPWindow *window)
{
	HevPluginPrivate *priv = (HevPluginPrivate*)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);
	
	g_return_val_if_fail(instance, NPERR_INVALID_INSTANCE_ERROR);

	return NPERR_NO_ERROR;
}

NPError NPP_NewStream(NPP instance, NPMIMEType type,
			NPStream *stream, NPBool seekable, uint16_t *stype)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return NPERR_NO_ERROR;
}

NPError NPP_DestroyStream(NPP instance, NPStream *stream,
			NPReason reason)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return NPERR_NO_ERROR;
}

int32_t NPP_WriteReady(NPP instance, NPStream *stream)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	g_return_val_if_fail(instance, NPERR_INVALID_INSTANCE_ERROR);

	NPN_DestroyStream(instance, stream, NPRES_DONE);

	return -1L;
}

int32_t NPP_Write(NPP instance, NPStream *stream,
			int32_t offset, int32_t len, void *buffer)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	g_return_val_if_fail(instance, NPERR_INVALID_INSTANCE_ERROR);

	NPN_DestroyStream(instance, stream, NPRES_DONE);

	return -1L;
}

void NPP_StreamAsFile(NPP instance, NPStream *stream,
			const char *fname)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);
}

void NPP_Print(NPP instance, NPPrint *platform_print)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);
}

int16_t NPP_HandleEvent(NPP instance, void *event)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	return 0;
}

void NPP_URLNotify(NPP instance, const char *url, NPReason reason,
			void *notify_data)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);
}

NPError NPP_GetValue(NPP instance, NPPVariable variable,
			void *value)
{
	HevPluginPrivate *priv = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	if(instance)
	  priv = (HevPluginPrivate *)instance->pdata;

	switch(variable)
	{
	case NPPVpluginNameString:
		*((char **)value) = PLUGIN_NAME;
		break;
	case NPPVpluginDescriptionString:
		*((char **)value) = PLUGIN_DESCRIPTION;
		break;
	case NPPVpluginNeedsXEmbed:
		*((PRBool *)value) = PR_TRUE;
		break;
	case NPPVpluginScriptableNPObject:
		if(NULL == priv->obj)
			priv->obj = NPN_CreateObject(instance, &npklass);
		else
		  priv->obj = NPN_RetainObject(priv->obj);
		*((NPObject **)value)  = priv->obj;
		break;
	default:
		return NPERR_GENERIC_ERROR;
	}

	return NPERR_NO_ERROR;
}

static NPObject * NPO_Allocate(NPP npp, NPClass *klass)
{
	HevScriptableObj *obj = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	obj = g_slice_new0(HevScriptableObj);
	obj->obj._class = klass;
	obj->npp = npp;

	return (NPObject *)obj;
}

static void NPO_Deallocate(NPObject *npobj)
{
	HevScriptableObj *obj = (HevScriptableObj *)npobj;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	g_slice_free(HevScriptableObj, obj);
}

static bool NPO_Invoke(NPObject *npobj, NPIdentifier name,
			const NPVariant *args, uint32_t arg_count,
			NPVariant *result)
{
	HevScriptableObj *obj = (HevScriptableObj *)npobj;
	HevPluginPrivate *priv = (HevPluginPrivate *)obj->npp->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	if(name == npi_get_reader_status)
	{
		BOOLEAN_TO_NPVARIANT(priv->reader_status, *result);
		return PR_TRUE;
	}
	else if(name == npi_get_card_status)
	{
		BOOLEAN_TO_NPVARIANT(priv->card_status, *result);
		return PR_TRUE;
	}
	else if(name == npi_get_card_info)
	{
		NPString key = { 0 };
		GVariant *val = NULL;

		if(1 != arg_count) return PR_FALSE;
		if(!NPVARIANT_IS_STRING(args[0])) return PR_FALSE;

		key = NPVARIANT_TO_STRING(args[0]);
		val = g_variant_lookup_value(priv->card_info,
					key.UTF8Characters, NULL);
		if(val)
		  STRINGZ_TO_NPVARIANT(g_variant_get_string(val, NULL),
					  *result);
		else
		  NULL_TO_NPVARIANT(*result);

		return PR_TRUE;
	}
	else if(name == npi_set_reader_status_notify_handler)
	{
		if(1 != arg_count) return PR_FALSE;

		/* Clear */
		if(priv->reader_status_notify_handler)
		{
			NPN_ReleaseObject(priv->reader_status_notify_handler);
			priv->reader_status_notify_handler = NULL;
		}

		/* Set */
		if(NPVARIANT_IS_OBJECT(args[0]))
		{
			priv->reader_status_notify_handler =
				NPVARIANT_TO_OBJECT(args[0]);
			NPN_RetainObject(priv->reader_status_notify_handler);
		}

		return PR_TRUE;
	}
	else if(name == npi_set_card_status_notify_handler)
	{
		if(1 != arg_count) return PR_FALSE;

		/* Clear */
		if(priv->card_status_notify_handler)
		{
			NPN_ReleaseObject(priv->card_status_notify_handler);
			priv->card_status_notify_handler = NULL;
		}

		/* Set */
		if(NPVARIANT_IS_OBJECT(args[0]))
		{
			priv->card_status_notify_handler =
				NPVARIANT_TO_OBJECT(args[0]);
			NPN_RetainObject(priv->card_status_notify_handler);
		}

		return PR_TRUE;
	}
	else if(name == npi_set_card_info_notify_handler)
	{
		if(1 != arg_count) return PR_FALSE;

		/* Clear */
		if(priv->card_info_notify_handler)
		{
			NPN_ReleaseObject(priv->card_info_notify_handler);
			priv->card_info_notify_handler = NULL;
		}

		/* Set */
		if(NPVARIANT_IS_OBJECT(args[0]))
		{
			priv->card_info_notify_handler =
				NPVARIANT_TO_OBJECT(args[0]);
			NPN_RetainObject(priv->card_info_notify_handler);
		}

		return PR_TRUE;
	}

	return PR_FALSE;
}

static bool NPO_HasMethod(NPObject *npobj, NPIdentifier method_name)
{
	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	if(method_name == npi_get_reader_status)
	  return PR_TRUE;
	else if(method_name == npi_get_card_status)
	  return PR_TRUE;
	else if(method_name == npi_get_card_info)
	  return PR_TRUE;
	else if(method_name == npi_set_reader_status_notify_handler)
	  return PR_TRUE;
	else if(method_name == npi_set_card_status_notify_handler)
	  return PR_TRUE;
	else if(method_name == npi_set_card_info_notify_handler)
	  return PR_TRUE;

	return PR_FALSE;
}

static void g_debug_log_null_handler(const gchar *domain,
			GLogLevelFlags level, const gchar *message,
			gpointer user_data)
{
}

static void g_bus_get_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	priv->dbus_conn = g_bus_get_finish(res, NULL);
	if(priv->dbus_conn)
	{
		g_object_set_data(G_OBJECT(priv->dbus_conn), "npp", instance);
		
		/* DBus proxy manager */
		g_dbus_proxy_new(priv->dbus_conn,
					G_DBUS_PROXY_FLAGS_NONE, NULL,
					"hev.idcard.Reader",
					"/hev/idcard/Reader/Manager",
					"hev.idcard.Reader.Manager",
					NULL, g_dbus_proxy_manager_new_handler,
					instance);
	}
}

static void g_dbus_proxy_manager_new_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	priv->dbus_proxy_manager = g_dbus_proxy_new_finish(res, NULL);
	if(priv->dbus_proxy_manager)
	{
		g_object_set_data(G_OBJECT(priv->dbus_proxy_manager), "npp", instance);

		/* Connect signal */
		g_signal_connect(priv->dbus_proxy_manager, "g-signal",
					G_CALLBACK(g_dbus_proxy_manager_g_signal_handler),
					instance);

		/* Enumerate devices */
		g_dbus_proxy_call(priv->dbus_proxy_manager, "EnumerateDevices",
					NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL,
					g_dbus_proxy_manager_enumerate_devices_handler,
					instance);
	}
}

static void g_dbus_proxy_manager_enumerate_devices_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;
	GVariant *retval = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	retval = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object),
				res, NULL);
	if(retval)
	{
		GVariant *devices = NULL;
		gsize i = 0, size = 0;

		devices = g_variant_get_child_value(retval, 0);
		size = g_variant_n_children(devices);
		for(i=0; i<size; i++)
		{
			GVariant *device = g_variant_get_child_value(devices, i);
			const gchar *path = g_variant_get_string(device, NULL);

			hev_plugin_add_reader(instance, path);
		}
		g_variant_unref(retval);
	}
}

static void g_dbus_proxy_reader_new_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;
	GDBusProxy *proxy = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	proxy = g_dbus_proxy_new_finish(res, NULL);
	if(proxy)
	{
		g_object_set_data(G_OBJECT(proxy), "npp", instance);

		/* Append to reader list */
		priv->dbus_proxy_readers = g_list_append(priv->dbus_proxy_readers,
					proxy);

		/* Connect signal */
		g_signal_connect(proxy, "g-signal",
					G_CALLBACK(g_dbus_proxy_reader_g_signal_handler),
					instance);

		/* Get reader status */
		g_dbus_proxy_call(G_DBUS_PROXY(proxy), "GetStatus",
					NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL,
					g_dbus_proxy_reader_get_status_handler,
					instance);

		/* Get card status */
		g_dbus_proxy_call(G_DBUS_PROXY(proxy), "GetCardStatus",
					NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL,
					g_dbus_proxy_reader_get_card_status_handler,
					instance);
	}
}

static void g_dbus_proxy_reader_get_status_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;
	GVariant *retval = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	retval = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object),
				res, NULL);
	if(retval)
	{
		GVariant *status = NULL;

		status = g_variant_get_child_value(retval, 0);
		priv->reader_status = g_variant_get_boolean(status);
		g_variant_unref(retval);
	}
}

static void g_dbus_proxy_reader_get_card_status_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;
	GVariant *retval = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	retval = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object),
				res, NULL);
	if(retval)
	{
		GVariant *status = NULL;

		status = g_variant_get_child_value(retval, 0);
		priv->card_status = g_variant_get_boolean(status);
		g_variant_unref(retval);
	}
}

static void g_dbus_proxy_reader_get_card_info_handler(GObject *source_object,
			GAsyncResult *res, gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;
	GVariant *retval = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	retval = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object),
				res, NULL);
	if(retval)
	{
		GVariant *info= NULL;

		info = g_variant_get_child_value(retval, 0);
		if(priv->card_info)
		  g_variant_unref(priv->card_info);
		priv->card_info = g_variant_ref(info);

		/* Notify */
		if(priv->card_info_notify_handler && priv->card_info)
		{
			NPVariant rval = { 0 };

			NPN_InvokeDefault(instance, priv->card_info_notify_handler,
						NULL, 0, &rval);
			NPN_ReleaseVariantValue(&rval);
		}

		g_variant_unref(retval);
	}
}

static void g_dbus_proxy_manager_g_signal_handler(GDBusProxy *proxy,
			gchar *sender_name, gchar *signal_name, GVariant *params,
			gpointer user_data)
{	
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	if(0 == g_strcmp0(signal_name, "Add"))
	{
		GVariant *v = NULL;
		const gchar *path = NULL;
		
		v = g_variant_get_child_value(params, 0);
		path = g_variant_get_string(v, NULL);
		hev_plugin_add_reader(instance, path);
	}
	else if(0 == g_strcmp0(signal_name, "Remove"))
	{
		GVariant *v = NULL;
		const gchar *path = NULL;
		
		v = g_variant_get_child_value(params, 0);
		path = g_variant_get_string(v, NULL);
		hev_plugin_remove_reader(instance, path);
	}
}

static void g_dbus_proxy_reader_g_signal_handler(GDBusProxy *proxy,
			gchar *sender_name, gchar *signal_name, GVariant *params,
			gpointer user_data)
{
	NPP instance = user_data;
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	if(0 == g_strcmp0(signal_name, "StatusChanged"))
	{
		GVariant *v = NULL;
		gboolean status = FALSE, s = FALSE;

		v = g_variant_get_child_value(params, 0);
		s = priv->reader_status;
		status = g_variant_get_boolean(v);
		priv->reader_status = status;
		/* Notify */
		if(priv->reader_status_notify_handler && (status!=s))
		{
			NPVariant val = { 0 }, rval = { 0 };

			BOOLEAN_TO_NPVARIANT(priv->reader_status, val);
			NPN_InvokeDefault(instance, priv->reader_status_notify_handler,
						&val, 1, &rval);
			NPN_ReleaseVariantValue(&val);
			NPN_ReleaseVariantValue(&rval);
		}
	}
	else if(0 == g_strcmp0(signal_name, "CardStatusChanged"))
	{
		GVariant *v = NULL;
		gboolean status = FALSE, s = FALSE;

		v = g_variant_get_child_value(params, 0);
		s = priv->card_status;
		status = g_variant_get_boolean(v);
		priv->card_status = status;
		/* Notify */
		if(priv->card_status_notify_handler && (status!=s))
		{
			NPVariant val = { 0 }, rval = { 0 };

			BOOLEAN_TO_NPVARIANT(priv->card_status, val);
			NPN_InvokeDefault(instance, priv->card_status_notify_handler,
						&val, 1, &rval);
			NPN_ReleaseVariantValue(&val);
			NPN_ReleaseVariantValue(&rval);
		}
	}
	else if(0 == g_strcmp0(signal_name, "CardInfoChanged"))
	{
		/* Get card info */
		g_dbus_proxy_call(G_DBUS_PROXY(proxy), "GetCardInfo",
					NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL,
					g_dbus_proxy_reader_get_card_info_handler,
					instance);
	}
}

static void hev_plugin_add_reader(NPP instance, const gchar *object_path)
{
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	/* DBus proxy reader */
	g_dbus_proxy_new(priv->dbus_conn,
				G_DBUS_PROXY_FLAGS_NONE, NULL,
				"hev.idcard.Reader",
				object_path,
				"hev.idcard.Reader",
				NULL, g_dbus_proxy_reader_new_handler,
				instance);
}

static void hev_plugin_remove_reader(NPP instance, const gchar *object_path)
{
	HevPluginPrivate *priv = (HevPluginPrivate *)instance->pdata;
	GList *sl = NULL;

	g_debug("%s:%d[%s]", __FILE__, __LINE__, __FUNCTION__);

	for(sl=priv->dbus_proxy_readers; sl; sl=g_list_next(sl))
	{
		GObject *obj = sl->data;
		gchar *path = NULL;
		gint t = 0;

		g_object_get(obj, "g-object-path", &path, NULL);
		t = g_strcmp0(path, object_path);
		g_free(path);

		if(0 == t)
		{
			g_object_unref(obj);
			priv->dbus_proxy_readers =
				g_list_remove(priv->dbus_proxy_readers, obj);
			/* Notify */
			if(0 == g_list_length(priv->dbus_proxy_readers))
			{
				/* Reader status */
				priv->reader_status = FALSE;
				if(priv->reader_status_notify_handler)
				{
					NPVariant val = { 0 }, rval = { 0 };

					BOOLEAN_TO_NPVARIANT(priv->reader_status, val);
					NPN_InvokeDefault(instance, priv->reader_status_notify_handler,
								&val, 1, &rval);
					NPN_ReleaseVariantValue(&val);
					NPN_ReleaseVariantValue(&rval);
				}
				/* Card status */
				priv->card_status = FALSE;
				if(priv->card_status_notify_handler)
				{
					NPVariant val = { 0 }, rval = { 0 };

					BOOLEAN_TO_NPVARIANT(priv->card_status, val);
					NPN_InvokeDefault(instance, priv->card_status_notify_handler,
								&val, 1, &rval);
					NPN_ReleaseVariantValue(&val);
					NPN_ReleaseVariantValue(&rval);
				}
			}

			break;
		}
	}
}

