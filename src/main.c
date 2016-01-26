/* gcc main.c -std=gnu11 -O2 -march=native `pkg-config --cflags --libs alsa glib-2.0 libudev x11` -Wall -Wpedantic */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <libudev.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#define DATETIME_FORMAT "%H:%M"
#define DATETIME_BUFFER 6
#define SLEEP_MSECONDS  350

#define GLYPH_CLOCK    "\uf017"
#define GLYPH_BATTERY  "\uf0e7"
#define GLYPH_VOLUME   "\uf028"
#define GLYPH_WEATHER  "\uf185"
#define GLYPH_KEYBOARD "\uf11c"
#define GLYPH_MAIL     "\uf0e0"

const char *layouts[] = {"En", "Ru"};

int main(void) {
    Display *xdisplay;
    XkbEvent xevent;
    int opcode, xkbEventBase, xkbErrorBase, major, minor;
    int currentLayout = 0;

    time_t timestamp;
    char datetime[DATETIME_BUFFER + 1];
    
    int status;
    snd_mixer_t *amixer;
    snd_mixer_elem_t *amixer_elem;
    snd_mixer_selem_id_t *amixer_selem;
    long int volume, volumeMin, volumeMax, volumePercent;

    struct udev *udev;
    struct udev_monitor *udev_monitor;
    struct udev_device *udev_device;
    int udev_fd;

    int ps_current = 0, ps_total = 0;

    openlog(NULL, LOG_CONS|LOG_PERROR|LOG_PID, LOG_DAEMON);

    if (!(xdisplay = XOpenDisplay(NULL))) {
        syslog(LOG_ERR, "Can't open display: %s!\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (!XkbQueryExtension(xdisplay, &opcode, &xkbEventBase, &xkbErrorBase, &major, &minor)) {
        syslog(LOG_ERR, "X doesn't support a compatible Xkb!\n");
        return EXIT_FAILURE;
    }

    if (!XkbSelectEvents(xdisplay, XkbUseCoreKbd, XkbStateNotifyMask, XkbStateNotifyMask)) {
        syslog(LOG_ERR, "Could not set Xkb event mask!\n");
        return EXIT_FAILURE;
    }

    if ((status = snd_mixer_open(&amixer, 0)) < 0 || (status = snd_mixer_selem_register(amixer, NULL, NULL)) < 0 || (status = snd_mixer_attach(amixer, "default")) < 0) {
        syslog(LOG_ERR, "Alsa failed: %s!\n", snd_strerror(status));
        return EXIT_FAILURE;
    }

    if ((status = snd_mixer_load(amixer)) || (status = snd_mixer_selem_id_malloc(&amixer_selem)) < 0) {
        syslog(LOG_ERR, "Alsa failed: %s!\n", snd_strerror(status));
        return EXIT_FAILURE;
    }

    snd_mixer_selem_id_set_index(amixer_selem, 0);
    snd_mixer_selem_id_set_name(amixer_selem, "Master");

    amixer_elem = snd_mixer_find_selem(amixer, amixer_selem);
    snd_mixer_selem_get_playback_volume_range(amixer_elem, &volumeMin, &volumeMax);

    if (amixer_elem == NULL) {
        syslog(LOG_ERR, "Mixer simple element handle not found!\n");
        return EXIT_FAILURE;
    }

    udev = udev_new();
    if (udev == NULL) {
        syslog(LOG_ERR, "Can't create udev object!\n");
        return EXIT_FAILURE;
    }

    udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
    if (udev_monitor == NULL) {
        syslog(LOG_ERR, "Can't create udev monitor!\n");
        return EXIT_SUCCESS;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "power_supply", NULL) < 0) {
        syslog(LOG_ERR, "Could't watch power_supply events!\n");
        return EXIT_FAILURE;
    }
    
    if (udev_monitor_enable_receiving(udev_monitor) < 0) {
        syslog(LOG_ERR, "Could't bind udev monitor event!\n");
        return EXIT_FAILURE;
    }

    udev_fd = udev_monitor_get_fd(udev_monitor);
    
    while(1) {
        time(&timestamp);
        strftime(datetime, DATETIME_BUFFER, DATETIME_FORMAT, localtime(&timestamp));

        while(XPending(xdisplay)) {
            XNextEvent(xdisplay, &xevent.core);
            if (xevent.type == xkbEventBase && xevent.any.xkb_type == XkbStateNotify) {
                currentLayout = xevent.state.group;
            }
        }

        if ((status = snd_mixer_handle_events(amixer)) < 0) {
            syslog(LOG_ERR, "Alsa failed: %s!\n", snd_strerror(status));
            return EXIT_FAILURE;
        }

        if ((status = snd_mixer_selem_get_playback_volume(amixer_elem, SND_MIXER_SCHN_MONO, &volume)) < 0) {
            syslog(LOG_ERR, "Alsa failed: %s!\n", snd_strerror(status));
            return EXIT_FAILURE;
        }
        volumePercent = (volume * 100) / volumeMax;
        
        fd_set fds;
        int ret;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 0};

        FD_ZERO(&fds);
        FD_SET(udev_fd, &fds);
        ret = select(udev_fd + 1, &fds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(udev_fd, &fds)) {
            udev_device = udev_monitor_receive_device(udev_monitor);
            if (udev_device == NULL) {
                syslog(LOG_ERR, "Can't get udev device!\n");
                return EXIT_SUCCESS;
            }

            printf("Name: %s\n", udev_device_get_sysname(udev_device));
            printf("Node: %s\n", udev_device_get_devnode(udev_device));
            printf("Subsystem: %s\n", udev_device_get_subsystem(udev_device));
            printf("Devtype: %s\n", udev_device_get_devtype(udev_device));
            printf("Action: %s\n", udev_device_get_action(udev_device));

            /*ps_current = atoi(udev_device_get_property_value(udev_device, "POWER_SUPPLY_CHARGE_NOW"));*/
            /*ps_total = atoi(udev_device_get_property_value(udev_device, "POWER_SUPPLY_CHARGE_FULL"));*/
            printf("%s\n", udev_device_get_sysattr_value(udev_device, "energy_now"));
            printf("%s\n", udev_device_get_sysattr_value(udev_device, "energy_full"));
            udev_device_unref(udev_device);
        }


        printf("%s %s\t%s %li\t%s %i\t%s %s\n", 
                GLYPH_KEYBOARD, layouts[currentLayout],
                GLYPH_VOLUME, volumePercent,
                GLYPH_BATTERY, ps_current,
                GLYPH_CLOCK, datetime
        );

        usleep(SLEEP_MSECONDS * 1000);
    }

    snd_mixer_selem_id_free(amixer_selem);
    snd_mixer_close(amixer);

    XCloseDisplay(xdisplay);
    closelog();

    return EXIT_SUCCESS;
}
