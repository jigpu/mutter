/*
 * Wayland Support
 *
 * Copyright (C) 2015 Red Hat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#ifndef META_WAYLAND_TABLET_H
#define META_WAYLAND_TABLET_H

#include <wayland-server.h>

#include <glib.h>

#include "meta-wayland-types.h"
#include "meta-cursor-renderer.h"
#include "meta-wayland-tablet-tool.h"

struct _MetaWaylandTablet
{
  MetaWaylandTabletManager *manager;
  ClutterInputDevice *device;
  GHashTable *tools;

  MetaWaylandTabletTool *current_tool;

  struct wl_list resource_list;
  struct wl_list focus_resource_list;

  MetaWaylandSurface *focus_surface;
  struct wl_listener focus_surface_destroy_listener;

  MetaCursorRenderer *cursor_renderer;
  MetaCursorSprite *cursor;
  MetaWaylandSurface *cursor_surface;
  struct wl_listener cursor_surface_destroy_listener;
  int hotspot_x, hotspot_y;

  MetaWaylandSurface *current;
  GSList *buttons;

  guint32 proximity_serial;
};

MetaWaylandTablet * meta_wayland_tablet_new          (ClutterInputDevice       *device,
                                                      MetaWaylandTabletManager *manager);
void                meta_wayland_tablet_free         (MetaWaylandTablet        *tablet);

void                meta_wayland_tablet_update       (MetaWaylandTablet  *tablet,
                                                      const ClutterEvent *event);
gboolean            meta_wayland_tablet_handle_event (MetaWaylandTablet  *tablet,
                                                      const ClutterEvent *event);

struct wl_resource *
             meta_wayland_tablet_create_new_resource (MetaWaylandTablet  *tablet,
                                                      struct wl_client   *client,
                                                      struct wl_resource *seat_resource,
                                                      uint32_t            id);
struct wl_resource *
             meta_wayland_tablet_lookup_resource     (MetaWaylandTablet  *tablet,
                                                      struct wl_client   *client);

void         meta_wayland_tablet_update_cursor_position (MetaWaylandTablet *tablet,
                                                         int                new_x,
                                                         int                new_y);

#endif /* META_WAYLAND_TABLET_H */
