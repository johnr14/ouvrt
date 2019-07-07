/*
 * Device base class
 * Copyright 2015 Philipp Zabel
 * SPDX-License-Identifier:	LGPL-2.0+ or BSL-1.0
 */
#ifndef __DEVICE_H__
#define __DEVICE_H__

#include <glib.h>
#include <glib-object.h>

enum device_type {
	DEVICE_TYPE_HMD,
	DEVICE_TYPE_CAMERA,
	DEVICE_TYPE_CONTROLLER,
};

#define OUVRT_TYPE_DEVICE		(ouvrt_device_get_type())
#define OUVRT_DEVICE(obj)		(G_TYPE_CHECK_INSTANCE_CAST((obj), \
					 OUVRT_TYPE_DEVICE, OuvrtDevice))
#define OUVRT_IS_DEVICE(obj)		(G_TYPE_CHECK_INSTANCE_TYPE((obj), \
					 OUVRT_TYPE_DEVICE))
#define OUVRT_DEVICE_CLASS(klass)	(G_TYPE_CHECK_CLASS_CAST((klass), \
					 OUVRT_TYPE_DEVICE, OuvrtDeviceClass))
#define OUVRT_IS_DEVICE_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE((klass), \
					 OUVRT_TYPE_DEVICE))
#define OUVRT_DEVICE_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS((obj), \
					 OUVRT_TYPE_DEVICE, OuvrtDeviceClass))

typedef struct _OuvrtDevice		OuvrtDevice;
typedef struct _OuvrtDeviceClass	OuvrtDeviceClass;
typedef struct _OuvrtDevicePrivate	OuvrtDevicePrivate;

struct _OuvrtDevice {
	GObject parent_instance;

	unsigned long id;
	enum device_type type;
	union {
		char *devnode;
		char *devnodes[3];
	};
	char *name;
	char *serial;
	gboolean active;
	gboolean has_radio;
	union {
		int fd;
		int fds[3];
	};
	char *parent_devpath;

	OuvrtDevicePrivate *priv;
};

struct _OuvrtDeviceClass {
	GObjectClass parent_class;

	int (*open)(OuvrtDevice *dev);
	int (*start)(OuvrtDevice *dev);
	void (*thread)(OuvrtDevice *dev);
	void (*stop)(OuvrtDevice *dev);
	void (*close)(OuvrtDevice *dev);

	void (*radio_start_discovery)(OuvrtDevice *dev);
	void (*radio_stop_discovery)(OuvrtDevice *dev);
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC(OuvrtDevice, g_object_unref)

GType ouvrt_device_get_type(void);

unsigned long ouvrt_device_claim_id(OuvrtDevice *dev, const char *serial);
int ouvrt_device_open(OuvrtDevice *dev);
int ouvrt_device_start(OuvrtDevice *dev);
void ouvrt_device_stop(OuvrtDevice *dev);
void ouvrt_device_close(OuvrtDevice *dev);

void ouvrt_device_radio_start_discovery(OuvrtDevice *dev);
void ouvrt_device_radio_stop_discovery(OuvrtDevice *dev);

#endif /* __DEVICE_H__ */
