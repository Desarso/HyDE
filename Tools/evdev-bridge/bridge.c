#include <time.h>
/*
 * evdev-bridge: Reads evdev events from Sunshine's virtual input devices
 * and injects them into Hyprland via Wayland protocols:
 *   - zwlr_virtual_pointer_manager_v1 for mouse
 *   - zwp_virtual_keyboard_v1 for keyboard
 *
 * This is needed because Hyprland in an LXC container has no libinput
 * backend, so kernel /dev/input devices are invisible to it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "protocols/wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "protocols/virtual-keyboard-unstable-v1-client-protocol.h"

/* Globals */
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct zwlr_virtual_pointer_v1 *vpointer;
static struct zwp_virtual_keyboard_v1 *vkeyboard;
static struct wl_output *output;

static int screen_w = 1920;
static int screen_h = 1200;
static volatile int running = 1;

/* Device paths */
static char kbd_path[256];
static char rel_mouse_path[256];
static char abs_mouse_path[256];

static int kbd_fd = -1;
static int rel_mouse_fd = -1;
static int abs_mouse_fd = -1;

/* Get evdev device name */
static int get_device_name(int fd, char *buf, size_t len) {
    if (ioctl(fd, EVIOCGNAME(len), buf) < 0)
        return -1;
    return 0;
}

/* Find a /dev/input/event* device by name prefix */
static int find_device(const char *name_prefix, char *out_path, size_t path_len) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[256] = {0};
        get_device_name(fd, name, sizeof(name));
        close(fd);

        if (strncmp(name, name_prefix, strlen(name_prefix)) == 0) {
            /* For "Mouse passthrough" vs "Mouse passthrough (absolute)" */
            if (strcmp(name_prefix, "Mouse passthrough") == 0 &&
                strstr(name, "absolute") != NULL) {
                continue; /* Skip absolute, we want relative */
            }
            snprintf(out_path, path_len, "%s", path);
            closedir(dir);
            fprintf(stderr, "[bridge] Found '%s' at %s\n", name, path);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

/* Create an anonymous file for the keymap */
static int create_anon_file(size_t size) {
    char name[] = "/tmp/evdev-bridge-keymap-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return -1;
    unlink(name);
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Registry listener */
static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        fprintf(stderr, "[bridge] Bound wl_seat\n");
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        pointer_manager = wl_registry_bind(reg, name,
            &zwlr_virtual_pointer_manager_v1_interface, 1);
        fprintf(stderr, "[bridge] Bound zwlr_virtual_pointer_manager_v1\n");
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        keyboard_manager = wl_registry_bind(reg, name,
            &zwp_virtual_keyboard_manager_v1_interface, 1);
        fprintf(stderr, "[bridge] Bound zwp_virtual_keyboard_manager_v1\n");
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !output) {
        output = wl_registry_bind(reg, name, &wl_output_interface, 1);
        fprintf(stderr, "[bridge] Bound wl_output\n");
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* Set up the virtual keyboard with a keymap */
static int setup_keyboard(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        fprintf(stderr, "[bridge] Failed to create xkb context\n");
        return -1;
    }

    struct xkb_rule_names names = {
        .rules = "evdev",
        .model = "pc105",
        .layout = "us",
        .variant = NULL,
        .options = NULL,
    };

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        fprintf(stderr, "[bridge] Failed to create keymap\n");
        xkb_context_unref(ctx);
        return -1;
    }

    char *keymap_str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!keymap_str) {
        fprintf(stderr, "[bridge] Failed to get keymap string\n");
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return -1;
    }

    size_t keymap_size = strlen(keymap_str) + 1;
    int fd = create_anon_file(keymap_size);
    if (fd < 0) {
        fprintf(stderr, "[bridge] Failed to create keymap file\n");
        free(keymap_str);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return -1;
    }

    char *map = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        free(keymap_str);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return -1;
    }
    memcpy(map, keymap_str, keymap_size);
    munmap(map, keymap_size);

    zwp_virtual_keyboard_v1_keymap(vkeyboard,
        WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, keymap_size);

    close(fd);
    free(keymap_str);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    fprintf(stderr, "[bridge] Keyboard keymap loaded (us/pc105)\n");
    return 0;
}

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Process mouse events */
static void handle_mouse_event(struct input_event *ev) {
    uint32_t t = get_time_ms();

    switch (ev->type) {
    case EV_REL:
        switch (ev->code) {
        case REL_X:
            zwlr_virtual_pointer_v1_motion(vpointer, t,
                wl_fixed_from_int(ev->value), 0);
            break;
        case REL_Y:
            zwlr_virtual_pointer_v1_motion(vpointer, t,
                0, wl_fixed_from_int(ev->value));
            break;
        case REL_WHEEL:
            zwlr_virtual_pointer_v1_axis(vpointer, t,
                WL_POINTER_AXIS_VERTICAL_SCROLL,
                wl_fixed_from_int(-ev->value * 15));
            zwlr_virtual_pointer_v1_axis_stop(vpointer, t,
                WL_POINTER_AXIS_VERTICAL_SCROLL);
            break;
        case REL_HWHEEL:
            zwlr_virtual_pointer_v1_axis(vpointer, t,
                WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                wl_fixed_from_int(ev->value * 15));
            zwlr_virtual_pointer_v1_axis_stop(vpointer, t,
                WL_POINTER_AXIS_HORIZONTAL_SCROLL);
            break;
        }
        break;

    case EV_KEY:
        if (ev->code >= BTN_LEFT && ev->code <= BTN_MIDDLE) {
            uint32_t button;
            switch (ev->code) {
            case BTN_LEFT:   button = 0x110; break; /* BTN_LEFT */
            case BTN_RIGHT:  button = 0x111; break;
            case BTN_MIDDLE: button = 0x112; break;
            default: return;
            }
            zwlr_virtual_pointer_v1_button(vpointer, t, button,
                ev->value ? WL_POINTER_BUTTON_STATE_PRESSED :
                           WL_POINTER_BUTTON_STATE_RELEASED);
        }
        break;

    case EV_SYN:
        if (ev->code == SYN_REPORT) {
            zwlr_virtual_pointer_v1_frame(vpointer);
            wl_display_flush(display);
        }
        break;
    }
}

/* Process absolute mouse events */
static void handle_abs_mouse_event(struct input_event *ev) {
    static int32_t ax = -1, ay = -1;
    uint32_t t = get_time_ms();

    switch (ev->type) {
    case EV_ABS:
        switch (ev->code) {
        case ABS_X:
            ax = ev->value;
            break;
        case ABS_Y:
            ay = ev->value;
            break;
        }
        break;

    case EV_KEY:
        if (ev->code >= BTN_LEFT && ev->code <= BTN_MIDDLE) {
            uint32_t button;
            switch (ev->code) {
            case BTN_LEFT:   button = 0x110; break;
            case BTN_RIGHT:  button = 0x111; break;
            case BTN_MIDDLE: button = 0x112; break;
            default: return;
            }
            zwlr_virtual_pointer_v1_button(vpointer, t, button,
                ev->value ? WL_POINTER_BUTTON_STATE_PRESSED :
                           WL_POINTER_BUTTON_STATE_RELEASED);
        }
        break;

    case EV_SYN:
        if (ev->code == SYN_REPORT) {
            if (ax >= 0 && ay >= 0) {
                /* Sunshine absolute range is 0-65535, map to screen */
                zwlr_virtual_pointer_v1_motion_absolute(vpointer, t,
                    (uint32_t)ax, (uint32_t)ay, 65535, 65535);
                ax = -1;
                ay = -1;
            }
            zwlr_virtual_pointer_v1_frame(vpointer);
            wl_display_flush(display);
        }
        break;
    }
}

/* Process keyboard events */
static void handle_keyboard_event(struct input_event *ev) {
    if (ev->type == EV_KEY) {
        uint32_t t = get_time_ms();
        /* evdev keycode + 8 = XKB keycode, but virtual keyboard protocol
         * uses evdev keycodes directly (offset handled by compositor) */
        zwp_virtual_keyboard_v1_key(vkeyboard, t, ev->code,
            ev->value ? WL_KEYBOARD_KEY_STATE_PRESSED :
                       WL_KEYBOARD_KEY_STATE_RELEASED);
        wl_display_flush(display);
    } else if (ev->type == EV_MSC) {
        /* Ignore MSC_SCAN etc */
    }
}

static void sighandler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    fprintf(stderr, "[bridge] evdev-bridge v2: native Wayland input injection\n");

    /* Connect to Wayland */
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "[bridge] Cannot connect to Wayland display. Set WAYLAND_DISPLAY.\n");
        return 1;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!pointer_manager) {
        fprintf(stderr, "[bridge] FATAL: zwlr_virtual_pointer_manager_v1 not available\n");
        return 1;
    }
    if (!keyboard_manager) {
        fprintf(stderr, "[bridge] FATAL: zwp_virtual_keyboard_manager_v1 not available\n");
        return 1;
    }
    if (!seat) {
        fprintf(stderr, "[bridge] FATAL: wl_seat not available\n");
        return 1;
    }

    /* Create virtual pointer */
    vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
        pointer_manager, seat, output);
    if (!vpointer) {
        fprintf(stderr, "[bridge] Failed to create virtual pointer\n");
        return 1;
    }
    fprintf(stderr, "[bridge] Virtual pointer created\n");

    /* Create virtual keyboard */
    vkeyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        keyboard_manager, seat);
    if (!vkeyboard) {
        fprintf(stderr, "[bridge] Failed to create virtual keyboard\n");
        return 1;
    }
    fprintf(stderr, "[bridge] Virtual keyboard created\n");

    /* Upload keymap */
    if (setup_keyboard() < 0) {
        fprintf(stderr, "[bridge] Failed to setup keyboard\n");
        return 1;
    }

    wl_display_flush(display);

    /* Wait for Sunshine devices */
    fprintf(stderr, "[bridge] Looking for Sunshine input devices...\n");
    while (running) {
        if (find_device("Keyboard passthrough", kbd_path, sizeof(kbd_path)) == 0 &&
            find_device("Mouse passthrough", rel_mouse_path, sizeof(rel_mouse_path)) == 0) {
            find_device("Mouse passthrough (absolute)", abs_mouse_path, sizeof(abs_mouse_path));
            break;
        }
        fprintf(stderr, "[bridge] Waiting for Sunshine devices...\n");
        sleep(2);
    }

    /* Open devices */
    kbd_fd = open(kbd_path, O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0) {
        fprintf(stderr, "[bridge] Cannot open keyboard %s: %s\n", kbd_path, strerror(errno));
        return 1;
    }

    rel_mouse_fd = open(rel_mouse_path, O_RDONLY | O_NONBLOCK);
    if (rel_mouse_fd < 0) {
        fprintf(stderr, "[bridge] Cannot open mouse %s: %s\n", rel_mouse_path, strerror(errno));
        return 1;
    }

    if (abs_mouse_path[0]) {
        abs_mouse_fd = open(abs_mouse_path, O_RDONLY | O_NONBLOCK);
        if (abs_mouse_fd < 0) {
            fprintf(stderr, "[bridge] Cannot open abs mouse %s: %s\n", abs_mouse_path, strerror(errno));
            /* Non-fatal */
        }
    }

    fprintf(stderr, "[bridge] All devices open. Forwarding events...\n");

    /* Event loop */
    while (running) {
        struct pollfd fds[4];
        int nfds = 0;

        fds[nfds].fd = kbd_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        fds[nfds].fd = rel_mouse_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        if (abs_mouse_fd >= 0) {
            fds[nfds].fd = abs_mouse_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        /* Also poll Wayland display fd for events */
        fds[nfds].fd = wl_display_get_fd(display);
        fds[nfds].events = POLLIN;
        nfds++;

        int ret = poll(fds, nfds, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Process Wayland events */
        wl_display_dispatch_pending(display);

        /* Read evdev events */
        struct input_event ev;
        int idx = 0;

        /* Keyboard */
        if (fds[idx].revents & POLLIN) {
            while (read(kbd_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                handle_keyboard_event(&ev);
            }
        }
        idx++;

        /* Relative mouse */
        if (fds[idx].revents & POLLIN) {
            while (read(rel_mouse_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                handle_mouse_event(&ev);
            }
        }
        idx++;

        /* Absolute mouse */
        if (abs_mouse_fd >= 0) {
            if (fds[idx].revents & POLLIN) {
                while (read(abs_mouse_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                    handle_abs_mouse_event(&ev);
                }
            }
            idx++;
        }

        wl_display_flush(display);
    }

    fprintf(stderr, "[bridge] Shutting down...\n");

    if (kbd_fd >= 0) close(kbd_fd);
    if (rel_mouse_fd >= 0) close(rel_mouse_fd);
    if (abs_mouse_fd >= 0) close(abs_mouse_fd);

    if (vpointer) zwlr_virtual_pointer_v1_destroy(vpointer);
    if (vkeyboard) zwp_virtual_keyboard_v1_destroy(vkeyboard);
    if (pointer_manager) zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);

    wl_display_disconnect(display);
    return 0;
}
