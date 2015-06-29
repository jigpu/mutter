/*
 * Copyright © 2011 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/* The file is based on src/data-device.c from Weston */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

#include "meta-wayland-data-device.h"
#include "meta-wayland-seat.h"
#include "meta-wayland-pointer.h"
#include "meta-wayland-private.h"
#include "meta-dnd-actor-private.h"

struct _MetaWaylandDataOffer
{
  struct wl_resource *resource;
  MetaWaylandDataSource *source;
  struct wl_listener source_destroy_listener;
  uint32_t dnd_actions;
  uint32_t preferred_dnd_action;
};

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static uint32_t
data_offer_choose_action (MetaWaylandDataOffer *offer)
{
  MetaWaylandDataSource *source = offer->source;
  uint32_t available_actions;

  available_actions = source->dnd_actions & offer->dnd_actions;

  if (!available_actions)
    return 0;

  /* If the user is forcing an action, go for it */
  if ((source->user_dnd_action & available_actions) != 0)
    return source->user_dnd_action;

  /* If the dest side has a preferred DnD action, use it */
  if ((offer->preferred_dnd_action & available_actions) != 0)
    return offer->preferred_dnd_action;

  /* Use the first found action, in bit order */
  return 1 << (ffs (available_actions) - 1);
}

void
meta_wayland_data_source_set_current_action (MetaWaylandDataSource *source,
                                             uint32_t               action)
{
  if (source->current_dnd_action == action)
    return;

  source->current_dnd_action = action;
  source->funcs.action (source, action);
}

static void
data_offer_update_action (MetaWaylandDataOffer *offer)
{
  uint32_t action;

  if (!offer->source)
    return;

  action = data_offer_choose_action (offer);

  if (offer->source->current_dnd_action == action)
    return;

  meta_wayland_data_source_set_current_action (offer->source, action);
  wl_data_offer_send_action (offer->resource, action);
}

static void
data_offer_accept (struct wl_client *client,
                   struct wl_resource *resource,
                   guint32 serial,
                   const char *mime_type)
{
  MetaWaylandDataOffer *offer = wl_resource_get_user_data (resource);

  /* FIXME: Check that client is currently focused by the input
   * device that is currently dragging this data source.  Should
   * this be a wl_data_device request? */

  if (offer->source)
    {
      offer->source->funcs.target (offer->source, mime_type);
      offer->source->has_target = mime_type != NULL;
    }
}

static void
data_offer_receive (struct wl_client *client, struct wl_resource *resource,
                    const char *mime_type, int32_t fd)
{
  MetaWaylandDataOffer *offer = wl_resource_get_user_data (resource);

  if (offer->source)
    meta_wayland_data_source_send (offer->source, mime_type, fd);
  else
    close (fd);
}

static void
data_offer_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_offer_set_actions (struct wl_client   *client,
                        struct wl_resource *resource,
                        uint32_t            dnd_actions,
                        uint32_t            preferred_action)
{
  MetaWaylandDataOffer *offer = wl_resource_get_user_data (resource);

  if (offer->dnd_actions == dnd_actions &&
      offer->preferred_dnd_action == preferred_action)
    return;

  offer->dnd_actions = dnd_actions;
  offer->preferred_dnd_action = preferred_action;

  data_offer_update_action (offer);
}

static const struct wl_data_offer_interface data_offer_interface = {
  data_offer_accept,
  data_offer_receive,
  data_offer_destroy,
  data_offer_set_actions
};

void
meta_wayland_data_source_notify_finish (MetaWaylandDataSource *source)
{
  source->funcs.drag_finished (source);
}

static void
destroy_data_offer (struct wl_resource *resource)
{
  MetaWaylandDataOffer *offer = wl_resource_get_user_data (resource);

  if (offer->source)
    {
      if (offer == offer->source->offer)
        meta_wayland_data_source_notify_finish (offer->source);

      if (offer->source->resource)
        wl_list_remove (&offer->source_destroy_listener.link);

      offer->source = NULL;
    }

  meta_display_sync_wayland_input_focus (meta_get_display ());
  g_slice_free (MetaWaylandDataOffer, offer);
}

static void
destroy_offer_data_source (struct wl_listener *listener, void *data)
{
  MetaWaylandDataOffer *offer;

  offer = wl_container_of (listener, offer, source_destroy_listener);
  offer->source = NULL;
}

static struct wl_resource *
meta_wayland_data_source_send_offer (MetaWaylandDataSource *source,
                                     struct wl_resource *target)
{
  MetaWaylandDataOffer *offer = g_slice_new0 (MetaWaylandDataOffer);
  char **p;

  offer->source = source;
  offer->resource = wl_resource_create (wl_resource_get_client (target), &wl_data_offer_interface, wl_resource_get_version (target), 0);
  wl_resource_set_implementation (offer->resource, &data_offer_interface, offer, destroy_data_offer);

  if (source->resource)
    {
      offer->source_destroy_listener.notify = destroy_offer_data_source;
      wl_resource_add_destroy_listener (source->resource, &offer->source_destroy_listener);
    }

  wl_data_device_send_data_offer (target, offer->resource);

  wl_array_for_each (p, &source->mime_types)
    wl_data_offer_send_offer (offer->resource, *p);

  data_offer_update_action (offer);
  source->offer = offer;

  return offer->resource;
}

static void
data_source_offer (struct wl_client *client,
                   struct wl_resource *resource, const char *type)
{
  MetaWaylandDataSource *source = wl_resource_get_user_data (resource);

  if (!meta_wayland_data_source_add_mime_type (source, type))
    wl_resource_post_no_memory (resource);
}

static void
data_source_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

void
meta_wayland_data_source_update_actions (MetaWaylandDataSource *source,
                                         uint32_t               dnd_actions)
{
  if (source->dnd_actions == dnd_actions)
    return;

  source->dnd_actions = dnd_actions;

  if (source->offer)
    {
      wl_data_offer_send_source_actions (source->offer->resource,
                                         source->dnd_actions);
      data_offer_update_action (source->offer);
    }
}

static void
data_source_set_actions (struct wl_client   *client,
                         struct wl_resource *resource,
                         uint32_t            dnd_actions)
{
  MetaWaylandDataSource *source = wl_resource_get_user_data (resource);

  meta_wayland_data_source_update_actions (source, dnd_actions);
}

static struct wl_data_source_interface data_source_interface = {
  data_source_offer,
  data_source_destroy,
  data_source_set_actions
};

struct _MetaWaylandDragGrab {
  MetaWaylandPointerGrab  generic;

  MetaWaylandKeyboardGrab keyboard_grab;

  MetaWaylandSeat        *seat;
  struct wl_client       *drag_client;

  MetaWaylandSurface     *drag_focus;
  struct wl_resource     *drag_focus_data_device;
  struct wl_listener      drag_focus_listener;

  MetaWaylandSurface     *drag_surface;
  struct wl_listener      drag_icon_listener;

  MetaWaylandDataSource  *drag_data_source;
  struct wl_listener      drag_data_source_listener;

  ClutterActor           *feedback_actor;

  MetaWaylandSurface     *drag_origin;
  struct wl_listener      drag_origin_listener;

  int                     drag_start_x, drag_start_y;
  ClutterModifierType     buttons;
};

static void
destroy_drag_focus (struct wl_listener *listener, void *data)
{
  MetaWaylandDragGrab *grab = wl_container_of (listener, grab, drag_focus_listener);

  grab->drag_focus_data_device = NULL;
  grab->drag_focus = NULL;
}

void
meta_wayland_drag_grab_set_focus (MetaWaylandDragGrab *drag_grab,
                                  MetaWaylandSurface  *surface)
{
  MetaWaylandSeat *seat = drag_grab->seat;
  struct wl_client *client;
  struct wl_resource *data_device_resource, *offer = NULL;

  if (drag_grab->drag_focus == surface)
    return;

  if (drag_grab->drag_focus)
    {
      meta_wayland_surface_drag_dest_focus_out (drag_grab->drag_focus);
      drag_grab->drag_focus = NULL;
    }

  if (!surface)
    return;

  if (!drag_grab->drag_data_source &&
      wl_resource_get_client (surface->resource) != drag_grab->drag_client)
    return;

  client = wl_resource_get_client (surface->resource);

  data_device_resource = wl_resource_find_for_client (&seat->data_device.resource_list, client);
  drag_grab->drag_data_source->offer = NULL;

  if (drag_grab->drag_data_source && data_device_resource)
    offer = meta_wayland_data_source_send_offer (drag_grab->drag_data_source,
                                                 data_device_resource);

  drag_grab->drag_focus = surface;
  drag_grab->drag_focus_data_device = data_device_resource;

  meta_wayland_surface_drag_dest_focus_in (drag_grab->drag_focus,
                                           offer ? wl_resource_get_user_data (offer) : NULL);
}

MetaWaylandSurface *
meta_wayland_drag_grab_get_focus (MetaWaylandDragGrab *drag_grab)
{
  return drag_grab->drag_focus;
}

static void
drag_grab_focus (MetaWaylandPointerGrab *grab,
                 MetaWaylandSurface     *surface)
{
  MetaWaylandDragGrab *drag_grab = (MetaWaylandDragGrab*) grab;

  meta_wayland_drag_grab_set_focus (drag_grab, surface);
}

static void
data_source_update_user_dnd_action (MetaWaylandDataSource *source,
                                    ClutterModifierType    modifiers)
{
  uint32_t user_dnd_action = 0;

  if (modifiers & CLUTTER_SHIFT_MASK)
    user_dnd_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
  else if (modifiers & CLUTTER_CONTROL_MASK)
    user_dnd_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
  else if (modifiers & (CLUTTER_MOD1_MASK | CLUTTER_BUTTON2_MASK))
    user_dnd_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;

  if (source->user_dnd_action == user_dnd_action)
    return;

  source->user_dnd_action = user_dnd_action;

  if (source->offer)
    data_offer_update_action (source->offer);
}

static void
drag_grab_motion (MetaWaylandPointerGrab *grab,
		  const ClutterEvent     *event)
{
  MetaWaylandDragGrab *drag_grab = (MetaWaylandDragGrab*) grab;

  if (drag_grab->drag_focus)
    meta_wayland_surface_drag_dest_motion (drag_grab->drag_focus, event);

  if (drag_grab->drag_surface)
    meta_feedback_actor_update (META_FEEDBACK_ACTOR (drag_grab->feedback_actor),
                                event);
}

static void
data_device_end_drag_grab (MetaWaylandDragGrab *drag_grab)
{
  meta_wayland_drag_grab_set_focus (drag_grab, NULL);

  if (drag_grab->drag_origin)
    {
      drag_grab->drag_origin = NULL;
      wl_list_remove (&drag_grab->drag_origin_listener.link);
    }

  if (drag_grab->drag_surface)
    {
      drag_grab->drag_surface = NULL;
      wl_list_remove (&drag_grab->drag_icon_listener.link);
    }

  if (drag_grab->drag_data_source &&
      drag_grab->drag_data_source->resource)
    wl_list_remove (&drag_grab->drag_data_source_listener.link);

  if (drag_grab->feedback_actor)
    {
      clutter_actor_remove_all_children (drag_grab->feedback_actor);
      clutter_actor_destroy (drag_grab->feedback_actor);
    }

  drag_grab->seat->data_device.current_grab = NULL;

  meta_wayland_pointer_end_grab (drag_grab->generic.pointer);
  meta_wayland_keyboard_end_grab (drag_grab->keyboard_grab.keyboard);
  g_slice_free (MetaWaylandDragGrab, drag_grab);
}

static void
drag_grab_button (MetaWaylandPointerGrab *grab,
		  const ClutterEvent     *event)
{
  MetaWaylandDragGrab *drag_grab = (MetaWaylandDragGrab*) grab;
  MetaWaylandSeat *seat = drag_grab->seat;
  ClutterEventType event_type = clutter_event_type (event);

  if (drag_grab->generic.pointer->grab_button == clutter_event_get_button (event) &&
      event_type == CLUTTER_BUTTON_RELEASE)
    {
      MetaWaylandDataSource *data_source = drag_grab->drag_data_source;
      gboolean success = FALSE;

      if (drag_grab->drag_focus &&
          data_source->has_target &&
          data_source->current_dnd_action)
        {
          meta_wayland_surface_drag_dest_drop (drag_grab->drag_focus);
          data_source->funcs.drop_performed (data_source);
          success = TRUE;
        }
      else
        {
          data_source->funcs.cancel (data_source);
        }

      /* Finish drag and let actor self-destruct */
      meta_dnd_actor_drag_finish (META_DND_ACTOR (drag_grab->feedback_actor),
                                  success);
      drag_grab->feedback_actor = NULL;
    }

  if (seat->pointer.button_count == 0 &&
      event_type == CLUTTER_BUTTON_RELEASE)
    data_device_end_drag_grab (drag_grab);
}

static const MetaWaylandPointerGrabInterface drag_grab_interface = {
  drag_grab_focus,
  drag_grab_motion,
  drag_grab_button,
};

static gboolean
keyboard_drag_grab_key (MetaWaylandKeyboardGrab *grab,
                        const ClutterEvent      *event)
{
  return FALSE;
}

static void
keyboard_drag_grab_modifiers (MetaWaylandKeyboardGrab *grab,
                              ClutterModifierType      modifiers)
{
  MetaWaylandDragGrab *drag_grab;

  drag_grab = wl_container_of (grab, drag_grab, keyboard_grab);

  /* The modifiers here just contain keyboard modifiers, mix it with the
   * mouse button modifiers we got when starting the drag operation.
   */
  modifiers |= drag_grab->buttons;

  if (drag_grab->drag_data_source)
    data_source_update_user_dnd_action (drag_grab->drag_data_source, modifiers);
}

static const MetaWaylandKeyboardGrabInterface keyboard_drag_grab_interface = {
  keyboard_drag_grab_key,
  keyboard_drag_grab_modifiers
};

static void
destroy_data_device_origin (struct wl_listener *listener, void *data)
{
  MetaWaylandDragGrab *drag_grab =
    wl_container_of (listener, drag_grab, drag_origin_listener);

  drag_grab->drag_origin = NULL;
  data_device_end_drag_grab (drag_grab);
  meta_wayland_data_device_set_dnd_source (&drag_grab->seat->data_device, NULL);
}

static void
destroy_data_device_source (struct wl_listener *listener, void *data)
{
  MetaWaylandDragGrab *drag_grab =
    wl_container_of (listener, drag_grab, drag_data_source_listener);

  drag_grab->drag_data_source = NULL;
  drag_grab->seat->data_device.dnd_data_source = NULL;
  data_device_end_drag_grab (drag_grab);
  meta_wayland_data_device_set_dnd_source (&drag_grab->seat->data_device, NULL);
}

static void
destroy_data_device_icon (struct wl_listener *listener, void *data)
{
  MetaWaylandDragGrab *drag_grab =
    wl_container_of (listener, drag_grab, drag_icon_listener);

  drag_grab->drag_surface = NULL;

  if (drag_grab->feedback_actor)
    clutter_actor_remove_all_children (drag_grab->feedback_actor);
}

void
meta_wayland_data_device_start_drag (MetaWaylandDataDevice                 *data_device,
                                     struct wl_client                      *client,
                                     const MetaWaylandPointerGrabInterface *funcs,
                                     MetaWaylandSurface                    *surface,
                                     MetaWaylandDataSource                 *source,
                                     MetaWaylandSurface                    *icon_surface)
{
  MetaWaylandSeat *seat = wl_container_of (data_device, seat, data_device);
  MetaWaylandDragGrab *drag_grab;
  ClutterPoint pos, stage_pos;
  ClutterModifierType modifiers;

  data_device->current_grab = drag_grab = g_slice_new0 (MetaWaylandDragGrab);

  drag_grab->generic.interface = funcs;
  drag_grab->generic.pointer = &seat->pointer;

  drag_grab->keyboard_grab.interface = &keyboard_drag_grab_interface;
  drag_grab->keyboard_grab.keyboard = &seat->keyboard;

  drag_grab->drag_client = client;
  drag_grab->seat = seat;

  drag_grab->drag_origin = surface;
  drag_grab->drag_origin_listener.notify = destroy_data_device_origin;
  wl_resource_add_destroy_listener (surface->resource,
                                    &drag_grab->drag_origin_listener);

  clutter_input_device_get_coords (seat->pointer.device, NULL, &pos);
  clutter_actor_transform_stage_point (CLUTTER_ACTOR (meta_surface_actor_get_texture (surface->surface_actor)),
                                       pos.x, pos.y, &stage_pos.x, &stage_pos.y);
  drag_grab->drag_start_x = stage_pos.x;
  drag_grab->drag_start_y = stage_pos.y;

  modifiers = clutter_input_device_get_modifier_state (seat->pointer.device);
  drag_grab->buttons = modifiers &
    (CLUTTER_BUTTON1_MASK | CLUTTER_BUTTON2_MASK | CLUTTER_BUTTON3_MASK |
     CLUTTER_BUTTON4_MASK | CLUTTER_BUTTON5_MASK);

  if (source)
    {
      if (source->resource)
        {
          drag_grab->drag_data_source_listener.notify = destroy_data_device_source;
          wl_resource_add_destroy_listener (source->resource,
                                            &drag_grab->drag_data_source_listener);
        }

      drag_grab->drag_data_source = source;
      meta_wayland_data_device_set_dnd_source (data_device,
                                               drag_grab->drag_data_source);
      data_source_update_user_dnd_action (drag_grab->drag_data_source, modifiers);
    }

  if (icon_surface)
    {
      drag_grab->drag_surface = icon_surface;

      drag_grab->drag_icon_listener.notify = destroy_data_device_icon;
      wl_resource_add_destroy_listener (icon_surface->resource,
                                        &drag_grab->drag_icon_listener);

      drag_grab->feedback_actor = meta_dnd_actor_new (CLUTTER_ACTOR (drag_grab->drag_origin->surface_actor),
                                                      drag_grab->drag_start_x,
                                                      drag_grab->drag_start_y);
      meta_feedback_actor_set_anchor (META_FEEDBACK_ACTOR (drag_grab->feedback_actor),
                                      -drag_grab->drag_surface->offset_x,
                                      -drag_grab->drag_surface->offset_y);
      clutter_actor_add_child (drag_grab->feedback_actor,
                               CLUTTER_ACTOR (drag_grab->drag_surface->surface_actor));

      meta_feedback_actor_set_position (META_FEEDBACK_ACTOR (drag_grab->feedback_actor),
                                        pos.x, pos.y);
    }

  meta_wayland_pointer_start_grab (&seat->pointer, (MetaWaylandPointerGrab*) drag_grab);
}

void
meta_wayland_data_device_end_drag (MetaWaylandDataDevice *data_device)
{
  if (data_device->current_grab)
    data_device_end_drag_grab (data_device->current_grab);
}

static void
data_device_start_drag (struct wl_client *client,
                        struct wl_resource *resource,
                        struct wl_resource *source_resource,
                        struct wl_resource *origin_resource,
                        struct wl_resource *icon_resource, guint32 serial)
{
  MetaWaylandDataDevice *data_device = wl_resource_get_user_data (resource);
  MetaWaylandSeat *seat = wl_container_of (data_device, seat, data_device);
  MetaWaylandSurface *surface = NULL, *icon_surface = NULL;
  MetaWaylandDataSource *drag_source = NULL;

  if (origin_resource)
    surface = wl_resource_get_user_data (origin_resource);

  if (!surface)
    return;

  if (seat->pointer.button_count == 0 ||
      seat->pointer.grab_serial != serial ||
      !seat->pointer.focus_surface ||
      seat->pointer.focus_surface != surface)
    return;

  /* FIXME: Check that the data source type array isn't empty. */

  if (data_device->current_grab ||
      seat->pointer.grab != &seat->pointer.default_grab)
    return;

  if (icon_resource)
    icon_surface = wl_resource_get_user_data (icon_resource);
  if (source_resource)
    drag_source = wl_resource_get_user_data (source_resource);

  if (icon_resource &&
      meta_wayland_surface_set_role (icon_surface,
                                     META_WAYLAND_SURFACE_ROLE_DND,
                                     resource,
                                     WL_DATA_DEVICE_ERROR_ROLE) != 0)
    return;

  meta_wayland_pointer_set_focus (&seat->pointer, NULL);
  meta_wayland_data_device_start_drag (data_device, client,
                                       &drag_grab_interface,
                                       surface, drag_source, icon_surface);

  meta_wayland_keyboard_set_focus (&seat->keyboard, NULL);
  meta_wayland_keyboard_start_grab (&seat->keyboard,
                                    &seat->data_device.current_grab->keyboard_grab);
}

static void
destroy_selection_data_source (struct wl_listener *listener, void *data)
{
  MetaWaylandDataDevice *data_device = wl_container_of (listener, data_device, selection_data_source_listener);
  MetaWaylandSeat *seat = wl_container_of (data_device, seat, data_device);
  struct wl_resource *data_device_resource;
  struct wl_client *focus_client = NULL;

  data_device->selection_data_source = NULL;

  focus_client = meta_wayland_keyboard_get_focus_client (&seat->keyboard);
  if (focus_client)
    {
      data_device_resource = wl_resource_find_for_client (&data_device->resource_list, focus_client);
      if (data_device_resource)
        wl_data_device_send_selection (data_device_resource, NULL);
    }
}

static void
meta_wayland_source_send (MetaWaylandDataSource *source,
                          const gchar           *mime_type,
                          gint                   fd)
{
  wl_data_source_send_send (source->resource, mime_type, fd);
  close (fd);
}

static void
meta_wayland_source_target (MetaWaylandDataSource *source,
                            const gchar           *mime_type)
{
  wl_data_source_send_target (source->resource, mime_type);
}

static void
meta_wayland_source_cancel (MetaWaylandDataSource *source)
{
  wl_data_source_send_cancelled (source->resource);
}

static void
meta_wayland_source_action (MetaWaylandDataSource *source,
                            uint32_t               action)
{
  wl_data_source_send_action (source->resource, action);
}

static void
meta_wayland_source_drop_performed (MetaWaylandDataSource *source)
{
  wl_data_source_send_drop_performed (source->resource);
}

static void
meta_wayland_source_drag_finished (MetaWaylandDataSource *source)
{
  wl_data_source_send_drag_finished (source->resource);
}

static const MetaWaylandDataSourceFuncs meta_wayland_source_funcs = {
  meta_wayland_source_send,
  meta_wayland_source_target,
  meta_wayland_source_cancel,
  meta_wayland_source_action,
  meta_wayland_source_drop_performed,
  meta_wayland_source_drag_finished
};

static void
meta_wayland_drag_dest_focus_in (MetaWaylandDataDevice *data_device,
                                 MetaWaylandSurface    *surface,
                                 MetaWaylandDataOffer  *offer)
{
  MetaWaylandDragGrab *grab = data_device->current_grab;
  struct wl_display *display;
  struct wl_client *client;
  wl_fixed_t sx, sy;

  if (!grab->drag_focus_data_device)
    return;

  client = wl_resource_get_client (surface->resource);
  display = wl_client_get_display (client);

  grab->drag_focus_listener.notify = destroy_drag_focus;
  wl_resource_add_destroy_listener (grab->drag_focus_data_device,
                                    &grab->drag_focus_listener);

  meta_wayland_pointer_get_relative_coordinates (grab->generic.pointer,
                                                 surface, &sx, &sy);
  wl_data_device_send_enter (grab->drag_focus_data_device,
                             wl_display_next_serial (display),
                             surface->resource, sx, sy, offer->resource);
}

static void
meta_wayland_drag_dest_focus_out (MetaWaylandDataDevice *data_device,
                                  MetaWaylandSurface    *surface)
{
  MetaWaylandDragGrab *grab = data_device->current_grab;

  if (grab->drag_focus_data_device)
    wl_data_device_send_leave (grab->drag_focus_data_device);

  wl_list_remove (&grab->drag_focus_listener.link);
  grab->drag_focus_data_device = NULL;
}

static void
meta_wayland_drag_dest_motion (MetaWaylandDataDevice *data_device,
                               MetaWaylandSurface    *surface,
                               const ClutterEvent    *event)
{
  MetaWaylandDragGrab *grab = data_device->current_grab;
  wl_fixed_t sx, sy;

  meta_wayland_pointer_get_relative_coordinates (grab->generic.pointer,
                                                 grab->drag_focus,
                                                 &sx, &sy);
  wl_data_device_send_motion (grab->drag_focus_data_device,
                              clutter_event_get_time (event),
                              sx, sy);
}

static void
meta_wayland_drag_dest_drop (MetaWaylandDataDevice *data_device,
                             MetaWaylandSurface    *surface)
{
  MetaWaylandDragGrab *grab = data_device->current_grab;

  wl_data_device_send_drop (grab->drag_focus_data_device);
}

static const MetaWaylandDragDestFuncs meta_wayland_drag_dest_funcs = {
  meta_wayland_drag_dest_focus_in,
  meta_wayland_drag_dest_focus_out,
  meta_wayland_drag_dest_motion,
  meta_wayland_drag_dest_drop
};

const MetaWaylandDragDestFuncs *
meta_wayland_data_device_get_drag_dest_funcs (void)
{
  return &meta_wayland_drag_dest_funcs;
}

void
meta_wayland_data_device_set_dnd_source (MetaWaylandDataDevice *data_device,
                                         MetaWaylandDataSource *source)
{
  if (data_device->dnd_data_source == source)
    return;

  if (data_device->dnd_data_source)
    meta_wayland_data_source_free (data_device->dnd_data_source);

  data_device->dnd_data_source = source;
  wl_signal_emit (&data_device->dnd_ownership_signal, source);
}

void
meta_wayland_data_device_set_selection (MetaWaylandDataDevice *data_device,
                                        MetaWaylandDataSource *source,
                                        guint32 serial)
{
  MetaWaylandSeat *seat = wl_container_of (data_device, seat, data_device);
  struct wl_resource *data_device_resource, *offer;
  struct wl_client *focus_client;

  if (data_device->selection_data_source &&
      data_device->selection_serial - serial < UINT32_MAX / 2)
    return;

  if (data_device->selection_data_source)
    {
      data_device->selection_data_source->funcs.cancel (data_device->selection_data_source);

      if (data_device->selection_data_source->resource)
        {
          wl_list_remove (&data_device->selection_data_source_listener.link);
          data_device->selection_data_source->resource = NULL;
        }

      meta_wayland_data_source_free (data_device->selection_data_source);
      data_device->selection_data_source = NULL;
    }

  data_device->selection_data_source = source;
  data_device->selection_serial = serial;

  focus_client = meta_wayland_keyboard_get_focus_client (&seat->keyboard);
  if (focus_client)
    {
      data_device_resource = wl_resource_find_for_client (&data_device->resource_list, focus_client);
      if (data_device_resource)
        {
          if (data_device->selection_data_source)
            {
              offer = meta_wayland_data_source_send_offer (data_device->selection_data_source, data_device_resource);
              wl_data_device_send_selection (data_device_resource, offer);
            }
          else
            {
              wl_data_device_send_selection (data_device_resource, NULL);
            }
        }
    }

  if (source)
    {
      if (source->resource)
        {
          data_device->selection_data_source_listener.notify = destroy_selection_data_source;
          wl_resource_add_destroy_listener (source->resource, &data_device->selection_data_source_listener);
        }
    }

  wl_signal_emit (&data_device->selection_ownership_signal, source);
}

static void
data_device_set_selection (struct wl_client *client,
                           struct wl_resource *resource,
                           struct wl_resource *source_resource,
                           guint32 serial)
{
  MetaWaylandDataDevice *data_device = wl_resource_get_user_data (resource);
  MetaWaylandDataSource *source;

  if (source_resource)
    source = wl_resource_get_user_data (source_resource);
  else
    source = NULL;

  /* FIXME: Store serial and check against incoming serial here. */
  meta_wayland_data_device_set_selection (data_device, source, serial);
}

static void
data_device_release(struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy(resource);
}

static const struct wl_data_device_interface data_device_interface = {
  data_device_start_drag,
  data_device_set_selection,
  data_device_release,
};

static void
destroy_data_source (struct wl_resource *resource)
{
  MetaWaylandDataSource *source = wl_resource_get_user_data (resource);

  source->resource = NULL;
}

static void
create_data_source (struct wl_client *client,
                    struct wl_resource *resource, guint32 id)
{
  MetaWaylandDataSource *source;
  struct wl_resource *source_resource;

  source_resource = wl_resource_create (client, &wl_data_source_interface,
                                        wl_resource_get_version (resource), id);
  source = meta_wayland_data_source_new (&meta_wayland_source_funcs,
                                         source_resource, NULL);
  wl_resource_set_implementation (source_resource, &data_source_interface,
                                  source, destroy_data_source);
}

static void
get_data_device (struct wl_client *client,
                 struct wl_resource *manager_resource,
                 guint32 id, struct wl_resource *seat_resource)
{
  MetaWaylandSeat *seat = wl_resource_get_user_data (seat_resource);
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_data_device_interface, wl_resource_get_version (manager_resource), id);
  wl_resource_set_implementation (cr, &data_device_interface, &seat->data_device, unbind_resource);
  wl_list_insert (&seat->data_device.resource_list, wl_resource_get_link (cr));
}

static const struct wl_data_device_manager_interface manager_interface = {
  create_data_source,
  get_data_device
};

static void
bind_manager (struct wl_client *client,
              void *data, guint32 version, guint32 id)
{
  struct wl_resource *resource;
  resource = wl_resource_create (client, &wl_data_device_manager_interface, version, id);
  wl_resource_set_implementation (resource, &manager_interface, NULL, NULL);
}

void
meta_wayland_data_device_manager_init (MetaWaylandCompositor *compositor)
{
  if (wl_global_create (compositor->wayland_display,
			&wl_data_device_manager_interface,
			META_WL_DATA_DEVICE_MANAGER_VERSION,
			NULL, bind_manager) == NULL)
    g_error ("Could not create data_device");
}

void
meta_wayland_data_device_init (MetaWaylandDataDevice *data_device)
{
  wl_list_init (&data_device->resource_list);
  wl_signal_init (&data_device->selection_ownership_signal);
  wl_signal_init (&data_device->dnd_ownership_signal);
}

void
meta_wayland_data_device_set_keyboard_focus (MetaWaylandDataDevice *data_device)
{
  MetaWaylandSeat *seat = wl_container_of (data_device, seat, data_device);
  struct wl_client *focus_client;
  struct wl_resource *data_device_resource, *offer;
  MetaWaylandDataSource *source;

  focus_client = meta_wayland_keyboard_get_focus_client (&seat->keyboard);
  if (!focus_client)
    return;

  data_device_resource = wl_resource_find_for_client (&data_device->resource_list, focus_client);
  if (!data_device_resource)
    return;

  source = data_device->selection_data_source;
  if (source)
    {
      offer = meta_wayland_data_source_send_offer (source, data_device_resource);
      wl_data_device_send_selection (data_device_resource, offer);
    }
  else
    wl_data_device_send_selection (data_device_resource, NULL);
}

gboolean
meta_wayland_data_device_is_dnd_surface (MetaWaylandDataDevice *data_device,
                                         MetaWaylandSurface    *surface)
{
  return data_device->current_grab &&
    data_device->current_grab->drag_surface == surface;
}

void
meta_wayland_data_device_update_dnd_surface (MetaWaylandDataDevice *data_device)
{
  MetaWaylandDragGrab *drag_grab;

  if (!data_device->current_grab)
    return;

  drag_grab = data_device->current_grab;

  if (!drag_grab->feedback_actor || !drag_grab->drag_surface)
    return;

  meta_feedback_actor_set_anchor (META_FEEDBACK_ACTOR (drag_grab->feedback_actor),
                                  -drag_grab->drag_surface->offset_x,
                                  -drag_grab->drag_surface->offset_y);
}

void
meta_wayland_data_source_send (MetaWaylandDataSource *source,
                               const gchar           *mime_type,
                               gint                   fd)
{
  source->funcs.send (source, mime_type, fd);
}

gboolean
meta_wayland_data_source_has_mime_type (const MetaWaylandDataSource *source,
                                        const gchar                 *mime_type)
{
  gchar **p;

  wl_array_for_each (p, &source->mime_types)
    {
      if (g_strcmp0 (mime_type, *p) == 0)
        return TRUE;
    }

  return FALSE;
}

MetaWaylandDataSource *
meta_wayland_data_source_new (const MetaWaylandDataSourceFuncs *funcs,
                              struct wl_resource               *wl_resource,
                              gpointer                          user_data)
{
  MetaWaylandDataSource *source = g_slice_new0 (MetaWaylandDataSource);

  source->funcs = *funcs;
  source->resource = wl_resource;
  source->user_data = user_data;
  wl_array_init (&source->mime_types);

  return source;
}

void
meta_wayland_data_source_free (MetaWaylandDataSource *source)
{
  char **pos;

  if (source->resource)
    wl_resource_destroy (source->resource);

  wl_array_for_each (pos, &source->mime_types)
    {
      g_free (*pos);
    }

  wl_array_release (&source->mime_types);
  g_slice_free (MetaWaylandDataSource, source);
}

gboolean
meta_wayland_data_source_add_mime_type (MetaWaylandDataSource *source,
                                        const gchar           *mime_type)
{
  gchar **pos;

  pos = wl_array_add (&source->mime_types, sizeof (*pos));

  if (pos)
    {
      *pos = g_strdup (mime_type);
      return *pos != NULL;
    }

  return FALSE;
}
