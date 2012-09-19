#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_event.h>
#include "helpers.h"
#include "types.h"
#include "bspwm.h"
#include "settings.h"
#include "utils.h"
#include "window.h"
#include "events.h"
#include "tree.h"
#include "rules.h"
#include "ewmh.h"

void handle_event(xcb_generic_event_t *evt)
{
    switch (XCB_EVENT_RESPONSE_TYPE(evt)) {
        case XCB_MAP_REQUEST:
            PUTS("map request\n");
            map_request(evt);
            break;
        case XCB_DESTROY_NOTIFY:
            PUTS("destroy notify\n");
            destroy_notify(evt);
            break;
        case XCB_UNMAP_NOTIFY:
            PUTS("unmap notify\n");
            unmap_notify(evt);
            break;
        case XCB_CLIENT_MESSAGE:
            PUTS("client message\n");
            client_message(evt);
            break;
        case XCB_CONFIGURE_REQUEST:
            PUTS("configure request\n");
            configure_request(evt);
            break;
        case XCB_BUTTON_PRESS:
            PUTS("button press\n");
            break;
        default:
            /* PRINTF("received event %i\n", XCB_EVENT_RESPONSE_TYPE(evt)); */
            break;
    }
}

void map_request(xcb_generic_event_t *evt)
{
    xcb_map_request_event_t *e = (xcb_map_request_event_t *) evt;
    xcb_get_window_attributes_reply_t  *wa;
    xcb_window_t win = e->window;
    window_location_t loc;
    wa = xcb_get_window_attributes_reply(dpy, xcb_get_window_attributes(dpy, win), NULL);

    if ((wa != NULL && wa->override_redirect) || locate_window(win, &loc))
        return;

    free(wa);

    client_t *c = make_client(win);

    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(dpy, xcb_get_geometry(dpy, win), NULL);

    if (geom) {
        c->rectangle = (xcb_rectangle_t) {geom->x, geom->y, geom->width, geom->height};
        free(geom);
    } else {
        c->rectangle = (xcb_rectangle_t) {0, 0, 320, 240};
    }

    bool floating = false, transient = false, takes_focus = true;

    handle_rules(win, &floating, &transient, &takes_focus);

    if (c->transient)
        floating = true;

    node_t *birth = make_node();
    birth->client = c;
    insert_node(desk, birth);

    if (floating)
        toggle_floating(birth);

    if (desk->focus != NULL && desk->focus->client->fullscreen)
        toggle_fullscreen(desk->focus->client);

    c->transient = transient;

    if (takes_focus)
        focus_node(desk, birth, false);

    apply_layout(desk, desk->root, root_rect);

    xcb_map_window(dpy, c->window);
    xcb_set_input_focus(dpy, XCB_INPUT_FOCUS_POINTER_ROOT, win, XCB_CURRENT_TIME);

    num_clients++;
    ewmh_update_client_list();
}

void configure_request(xcb_generic_event_t *evt)
{
    xcb_configure_request_event_t *e = (xcb_configure_request_event_t *) evt;
    window_location_t loc;
    bool is_managed = locate_window(e->window, &loc);

    if (!is_managed || is_floating(loc.node->client)) {
        uint16_t mask = 0;
        uint32_t values[7];
        unsigned short i = 0;

        if (e->value_mask & XCB_CONFIG_WINDOW_X) {
            mask |= XCB_CONFIG_WINDOW_X;
            values[i++] = e->x;
            if (is_managed)
                loc.node->client->rectangle.x = e->x;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_Y) {
            mask |= XCB_CONFIG_WINDOW_Y;
            values[i++] = e->y;
            if (is_managed)
                loc.node->client->rectangle.y = e->y;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            mask |= XCB_CONFIG_WINDOW_WIDTH;
            values[i++] = e->width;
            if (is_managed)
                loc.node->client->rectangle.width = e->width;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            mask |= XCB_CONFIG_WINDOW_HEIGHT;
            values[i++] = e->height;
            if (is_managed)
                loc.node->client->rectangle.height = e->height;
        }

        if (!is_managed && e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
            mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
            values[i++] = e->border_width;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[i++] = e->sibling;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            mask |= XCB_CONFIG_WINDOW_STACK_MODE;
            values[i++] = e->stack_mode;
        }

        xcb_configure_window(dpy, e->window, mask, values);
    }

    if (is_managed && is_floating(loc.node->client))
        apply_layout(loc.desktop, loc.node, root_rect);
}

void destroy_notify(xcb_generic_event_t *evt)
{
    xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *) evt;
    window_location_t loc;

    if (locate_window(e->window, &loc)) {
        remove_node(loc.desktop, loc.node);
        apply_layout(loc.desktop, loc.desktop->root, root_rect);
    }
}

void unmap_notify(xcb_generic_event_t *evt)
{
    xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *) evt;
    if (e->event != screen->root) {
        window_location_t loc;
        if (locate_window(e->window, &loc)) {
            remove_node(loc.desktop, loc.node);
            apply_layout(loc.desktop, loc.desktop->root, root_rect);
        }
    }
}

void client_message(xcb_generic_event_t *evt)
{
    xcb_client_message_event_t *e = (xcb_client_message_event_t *) evt;
    window_location_t loc;

    if (!locate_window(e->window, &loc))
        return;

    if (e->type == ewmh->_NET_WM_STATE) {
        handle_state(loc.node, e->data.data32[1], e->data.data32[0]);
        handle_state(loc.node, e->data.data32[2], e->data.data32[0]);
    } else if (e->type == ewmh->_NET_ACTIVE_WINDOW) {
        focus_node(loc.desktop, loc.node, true);
    }
}

void handle_state(node_t *n, xcb_atom_t state, unsigned int action)
{
    if (state == ewmh->_NET_WM_STATE_FULLSCREEN) {
        bool fs = n->client->fullscreen;
        if (action == XCB_EWMH_WM_STATE_TOGGLE
                || (fs && action == XCB_EWMH_WM_STATE_REMOVE)
                || (!fs && action == XCB_EWMH_WM_STATE_ADD))
            toggle_fullscreen(n->client);
    }
}
