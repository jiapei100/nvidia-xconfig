/*
 * nvidia-xconfig: A tool for manipulating X config files,
 * specifically for use by the NVIDIA Linux graphics driver.
 *
 * Copyright (C) 2005 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the:
 *
 *      Free Software Foundation, Inc.
 *      59 Temple Place - Suite 330
 *      Boston, MA 02111-1307, USA
 *
 *
 * multiple_screens.c
 */

#include "nvidia-xconfig.h"
#include "xf86Parser.h"

#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>


static int enable_separate_x_screens(Options *op, XConfigPtr config,
                                     XConfigLayoutPtr layout);
static int disable_separate_x_screens(Options *op, XConfigPtr config,
                                      XConfigLayoutPtr layout);

static int set_xinerama(int xinerama_enabled, XConfigPtr config);

static XConfigDisplayPtr clone_display_list(XConfigDisplayPtr display0);
static XConfigDevicePtr clone_device(XConfigDevicePtr device0);
static XConfigScreenPtr clone_screen(XConfigScreenPtr screen0);

static void create_adjacencies(Options *op, XConfigPtr config,
                               XConfigLayoutPtr layout);

static int enable_all_gpus(Options *op, XConfigPtr config,
                           XConfigLayoutPtr layout);

static void free_unused_devices(Options *op, XConfigPtr config);
static void free_unused_monitors(Options *op, XConfigPtr config);

static int only_one_screen(Options *op, XConfigPtr config,
                           XConfigLayoutPtr layout);

/*
 * apply_multi_screen_options() - there are 4 options that can affect
 * multiple X screens:
 *
 * - add X screens for all GPUS in the system
 * - separate X screens on one GPU (turned on or off)
 * - only one X screen
 * - Xinerama
 *
 * apply these options in that order
 */

int apply_multi_screen_options(Options *op, XConfigPtr config,
                               XConfigLayoutPtr layout)
{
    if (op->enable_all_gpus) {
        if (!enable_all_gpus(op, config, layout)) return FALSE;
    }
    

    if (GET_BOOL_OPTION(op->boolean_options,
                        SEPARATE_X_SCREENS_BOOL_OPTION)) {
        if (GET_BOOL_OPTION(op->boolean_option_values,
                            SEPARATE_X_SCREENS_BOOL_OPTION)) {
            if (!enable_separate_x_screens(op, config, layout)) return FALSE;
        } else {
            if (!disable_separate_x_screens(op, config, layout)) return FALSE;
        }
    }
    
    if (GET_BOOL_OPTION(op->boolean_options,
                        XINERAMA_BOOL_OPTION)) {
        if (!set_xinerama(GET_BOOL_OPTION(op->boolean_option_values,
                                          XINERAMA_BOOL_OPTION),
                          config)) return FALSE;
    }
    
    if (op->only_one_screen) {
        if (!only_one_screen(op, config, layout)) return FALSE;
    }
    
    return TRUE;
    
} /* apply_multi_screen_options() */



/*
 * find_devices() - dlopen the nvidia-cfg library and query the
 * available information about the GPUs in the system.
 */

DevicesPtr find_devices(Options *op)
{
    DevicesPtr pDevices = NULL;
    DisplayDevicePtr pDisplayDevice;
    int i, j, n, count = 0;
    unsigned int mask, bit;
    DeviceRec tmpDevice;
    NvCfgDeviceHandle handle;
    NvCfgDevice *devs = NULL;
    NvCfgBool is_primary_device;
    char *lib_path;
    void *lib_handle;

    NvCfgBool (*__getDevices)(int *n, NvCfgDevice **devs);
    NvCfgBool (*__openDevice)(int bus, int slot, NvCfgDeviceHandle *handle);
    NvCfgBool (*__getNumCRTCs)(NvCfgDeviceHandle handle, int *crtcs);
    NvCfgBool (*__getProductName)(NvCfgDeviceHandle handle, char **name);
    NvCfgBool (*__getDisplayDevices)(NvCfgDeviceHandle handle,
                                     unsigned int *display_device_mask);
    NvCfgBool (*__getEDID)(NvCfgDeviceHandle handle,
                           unsigned int display_device,
                           NvCfgDisplayDeviceInformation *info);
    NvCfgBool (*__isPrimaryDevice)(NvCfgDeviceHandle handle,
                                  NvCfgBool *is_primary_device);
    NvCfgBool (*__closeDevice)(NvCfgDeviceHandle handle);
    
    /* dlopen() the nvidia-cfg library */
    
#define __LIB_NAME "libnvidia-cfg.so.1"

    if (op->nvidia_cfg_path) {
        lib_path = nvstrcat(op->nvidia_cfg_path, "/", __LIB_NAME, NULL);
    } else {
        lib_path = strdup(__LIB_NAME);
    }
    
    lib_handle = dlopen(lib_path, RTLD_NOW);
    
    nvfree(lib_path);
    
    if (!lib_handle) {
        fmtwarn("error opening %s: %s.", __LIB_NAME, dlerror());
        return NULL;
    }
    
#define __GET_FUNC(proc, name)                                 \
    (proc) = dlsym(lib_handle, (name));                        \
    if (!(proc)) {                                             \
        fmtwarn("error retrieving symbol %s from %s: %s",      \
                (name), __LIB_NAME, dlerror());                \
        dlclose(lib_handle);                                   \
        return NULL;                                           \
    }

    /* required functions */
    __GET_FUNC(__getDevices, "nvCfgGetDevices");
    __GET_FUNC(__openDevice, "nvCfgOpenDevice");
    __GET_FUNC(__getNumCRTCs, "nvCfgGetNumCRTCs");
    __GET_FUNC(__getProductName, "nvCfgGetProductName");
    __GET_FUNC(__getDisplayDevices, "nvCfgGetDisplayDevices");
    __GET_FUNC(__getEDID, "nvCfgGetEDID");
    __GET_FUNC(__closeDevice, "nvCfgCloseDevice");

    /* optional functions */
    __isPrimaryDevice = dlsym(lib_handle, "nvCfgIsPrimaryDevice");
    
    if (__getDevices(&count, &devs) != NVCFG_TRUE) {
        return NULL;
    }

    if (count == 0) return NULL;

    pDevices = nvalloc(sizeof(DevicesRec));
    
    pDevices->devices = nvalloc(sizeof(DeviceRec) * count);

    pDevices->nDevices = count;

    for (i = 0; i < count; i++) {
        
        pDevices->devices[i].dev = devs[i];
        
        if (__openDevice(devs[i].bus, devs[i].slot, &handle) != NVCFG_TRUE)
            goto fail;
        
        if (__getNumCRTCs(handle, &pDevices->devices[i].crtcs) != NVCFG_TRUE)
            goto fail;
        
        if (__getProductName(handle, &pDevices->devices[i].name) != NVCFG_TRUE)
            goto fail;
        
        if (__getDisplayDevices(handle, &mask) != NVCFG_TRUE)
            goto fail;
        
        pDevices->devices[i].displayDeviceMask = mask;

        /* count the number of display devices */
        
        for (n = j = 0; j < 32; j++) {
            if (mask & (1 << j)) n++;
        }
        
        pDevices->devices[i].nDisplayDevices = n;

        if (n) {

            /* allocate the info array of the right size */
            
            pDevices->devices[i].displayDevices =
                nvalloc(sizeof(DisplayDeviceRec) * n);
            
            /* fill in the info array */
            
            for (n = j = 0; j < 32; j++) {
                bit = 1 << j;
                if (!(bit & mask)) continue;
                
                pDisplayDevice = &pDevices->devices[i].displayDevices[n];
                pDisplayDevice->mask = bit;

                if (__getEDID(handle, bit,
                              &pDisplayDevice->info) != NVCFG_TRUE) {
                    pDisplayDevice->info_valid = FALSE;
                } else {
                    pDisplayDevice->info_valid = TRUE;
                }
                n++;
            }
        } else {
            pDevices->devices[i].displayDevices = NULL;
        }

        if ((i != 0) && (__isPrimaryDevice != NULL) &&
            (__isPrimaryDevice(handle, &is_primary_device) == NVCFG_TRUE) &&
            (is_primary_device == NVCFG_TRUE)) {
            memcpy(&tmpDevice, &pDevices->devices[0], sizeof(DeviceRec));
            memcpy(&pDevices->devices[0], &pDevices->devices[i], sizeof(DeviceRec));
            memcpy(&pDevices->devices[i], &tmpDevice, sizeof(DeviceRec));
        }
        
        if (__closeDevice(handle) != NVCFG_TRUE)
            goto fail;
    }
    
    goto done;
    
 fail:

    fmtwarn("Unable to use the nvidia-cfg library to query NVIDIA "
            "hardware.");

    free_devices(pDevices);
    pDevices = NULL;

    /* fall through */

 done:
    
    if (devs) free(devs);
    
    return pDevices;
    
} /* find_devices() */



/*
 * free_devices()
 */

void free_devices(DevicesPtr pDevices)
{
    int i;
    
    if (!pDevices) return;
    
    for (i = 0; i < pDevices->nDevices; i++) {
        if (pDevices->devices[i].displayDevices) {
            nvfree(pDevices->devices[i].displayDevices);
        }
    }
    
    if (pDevices->devices) {
        nvfree(pDevices->devices);
    }
        
    nvfree(pDevices);
    
} /* free_devices() */



/*
 * set_xinerama() - This makes sure there is a ServerFlags
 * section and sets the "Xinerama" option
 */

static int set_xinerama(int xinerama_enabled, XConfigPtr config)
{
    if (!config->flags) {
        config->flags = nvalloc(sizeof(XConfigFlagsRec));
        if ( !config->flags ) {
            return FALSE;
        }
    }

    if (config->flags->options) {
        remove_option_from_list(&(config->flags->options), "Xinerama");
    }

    config->flags->options =
        xconfigAddNewOption(config->flags->options,
                            nvstrdup("Xinerama"),
                            nvstrdup(xinerama_enabled?"1":"0"));

    return TRUE;

} /* set_xinerama() */



/*
 * enable_separate_x_screens() - this effectively clones each screen
 * that is on a unique GPU.
 *
 * Algorithm:
 *
 * step 1: build a list of screens to be cloned
 *
 * step 2: assign a busID to every screen that is in the list (if
 * BusIDs are not already assigned)
 *
 * step 3: for every candidate screen, check if it is already one of
 * multiple screens on a gpu; if so, then it is not eligible for
 * cloning.  Note that this has to check all screens in the adjacency
 * list, not just the ones in the candidate list.
 *
 * step 4: clone each eligible screen
 * 
 * step 5: update adjacency list (just wipe the list and restart)
 *
 * XXX we need to check that there are actually 2 CRTCs on this GPU
 */

static int enable_separate_x_screens(Options *op, XConfigPtr config,
                                     XConfigLayoutPtr layout)
{
    XConfigScreenPtr screen, *screenlist = NULL;
    XConfigAdjacencyPtr adj;

    int i, nscreens = 0;
    int have_busids;
    
    /* build the list of screens that are candidate to be cloned */
    
    if (op->screen) {
        screen = xconfigFindScreen(op->screen, config->screens);
        if (!screen) {
            fmterr("Unable to find screen '%s'.", op->screen);
            return FALSE;
        }
        
        screenlist = nvalloc(sizeof(XConfigScreenPtr));
        screenlist[0] = screen;
        nscreens = 1;
        
    } else {
        for (adj = layout->adjacencies; adj; adj = adj->next) {
            nscreens++;
            screenlist = realloc(screenlist,
                                 sizeof(XConfigScreenPtr) * nscreens);
            
            screenlist[nscreens-1] = adj->screen;
        }
    }
    
    if (!nscreens) return FALSE;
    
    /* do all screens in the list have a bus ID? */
    
    have_busids = TRUE;
    
    for (i = 0; i < nscreens; i++) {
        if (screenlist[i] &&
            screenlist[i]->device &&
            screenlist[i]->device->busid) {
            // this screen has a busid
        } else {
            have_busids = FALSE;
            break;
        }
    }
    
    /*
     * if not everyone has a bus id, then let's assign bus ids to all
     * the screens; XXX what if _some_ already have busids?  Too bad,
     * we're going to reassign all of them.
     */
    
    if (!have_busids) {
        DevicesPtr pDevices;
        
        pDevices = find_devices(op);
        if (!pDevices) {
            fmterr("Unable to determine number or location of "
                   "GPUs in system; cannot "
                   "honor '--separate-x-screens' option.");
            return FALSE;
        }
        
        for (i = 0; i < nscreens; i++) {
            if (i >= pDevices->nDevices) {
                /*
                 * we have more screens than GPUs, this screen is no
                 * longer a candidate
                 */
                screenlist[i] = NULL;
                continue;
            }
            
            screenlist[i]->device->busid = nvalloc(32);
            snprintf(screenlist[i]->device->busid, 32,
                     "PCI:%d:%d:0",
                     pDevices->devices[i].dev.bus,
                     pDevices->devices[i].dev.slot);
            screenlist[i]->device->board = nvstrdup(pDevices->devices[i].name);
        }

        free_devices(pDevices);
    }
    
    /*
     * step 3: for every candidate screen, check if it is already one
     * of multiple screens on a gpu; if so, then it is not eligible
     * for cloning.  Note that this has to check all screens in the
     * adjacency list, not just the ones in the candidate list
     */
    
    for (i = 0; i < nscreens; i++) {
        int bus0, bus1, slot0, slot1, scratch;
        if (!screenlist[i]) continue;
        
        /* parse the bus id for this candidate screen */

        if (!xconfigParsePciBusString(screenlist[i]->device->busid,
                                      &bus0, &slot0, &scratch)) {
            /* parsing failed; this screen is no longer a candidate */
            screenlist[i] = NULL;
            continue;
        }
    
        /*
         * scan through all the screens; if any screen, other than
         * *this* screen, have the same busid, then this screen is no
         * longer a candidate
         */
        
        for (screen = config->screens; screen; screen = screen->next) {
            if (screen == screenlist[i]) continue;
            if (!screen->device) continue;
            if (!screen->device->busid) continue;
            if (!xconfigParsePciBusString(screen->device->busid,
                                          &bus1, &slot1, &scratch)) continue;
            if ((bus0 == bus1) && (slot0 == slot1)) {
                screenlist[i] = NULL; /* no longer a candidate */
                break;
            }
        }
    }
    
    /* clone each eligible screen */
    
    for (i = 0; i < nscreens; i++) {
        if (!screenlist[i]) continue;
        clone_screen(screenlist[i]);
    }
    
    /*
     * wipe the existing adjacencies and recreate them
     * 
     * XXX we should really only use the screens in the current
     * adjacency list, plus the new cloned screens, when building the
     * new adjacencies
     */
    
    xconfigFreeAdjacencyList(layout->adjacencies);
    layout->adjacencies = NULL;
    
    create_adjacencies(op, config, layout);

    /* free stuff */

    free(screenlist);

    return TRUE;

} /* enable_separate_x_screens() */


/*
 * disable_separate_x_screens() - remove multiple screens that are
 * configured for the same GPU.
 *
 * Algorithm:
 *
 * step 1: find which screens need to be "de-cloned" (either
 * op->screen or all screens in the layout)
 *
 * step 2: narrow that list down to screens that have a busid
 * specified
 *
 * step 3: find all other screens that have the same busid and remove
 * them
 *
 * step 3: recompute the adjacency list
 */

static int disable_separate_x_screens(Options *op, XConfigPtr config,
                                      XConfigLayoutPtr layout)
{
    XConfigScreenPtr screen, prev, next, *screenlist = NULL;
    XConfigAdjacencyPtr adj;
    
    int i, j, nscreens = 0;
    int *bus, *slot, scratch;
    
    /* build the list of screens that are candidate to be de-cloned */
    
    if (op->screen) {
        screen = xconfigFindScreen(op->screen, config->screens);
        if (!screen) {
            fmterr("Unable to find screen '%s'.", op->screen);
            return FALSE;
        }
        
        screenlist = nvalloc(sizeof(XConfigScreenPtr));
        screenlist[0] = screen;
        nscreens = 1;
        
    } else {
        for (adj = layout->adjacencies; adj; adj = adj->next) {
            nscreens++;
            screenlist = realloc(screenlist,
                                 sizeof(XConfigScreenPtr) * nscreens);
            
            screenlist[nscreens-1] = adj->screen;
        }
    }
    
    /*
     * limit the list to screens that have a BusID; parse the busIDs
     * while we're at it
     */
    
    bus = nvalloc(sizeof(int) * nscreens);
    slot = nvalloc(sizeof(int) * nscreens);
    
    for (i = 0; i < nscreens; i++) {
        if (screenlist[i] &&
            screenlist[i]->device &&
            screenlist[i]->device->busid &&
            xconfigParsePciBusString(screenlist[i]->device->busid,
                                     &bus[i], &slot[i], &scratch)) {
            // this screen has a valid busid
        } else {
            screenlist[i] = NULL;
        }
    }
    
    /* trim out duplicates */
    
    for (i = 0; i < nscreens; i++) {
        
        if (!screenlist[i]) continue;
        
        for (j = i+1; j < nscreens; j++) {
            if (!screenlist[j]) continue;
            if ((bus[i] == bus[j]) && (slot[i] == slot[j])) {
                screenlist[j] = NULL;
            }
        }
    }
    
    /*
     * for every screen in the de-clone list, scan through all
     * screens; if any screen, other than *this* screen has the same
     * busid, remove it
     */
    
    for (i = 0; i < nscreens; i++) {
        int bus0, slot0;
        if (!screenlist[i]) continue;

        screen = config->screens;
        prev = NULL;
        
        while (screen) {
            if (screenlist[i] == screen) goto next_screen;
            if (!screen->device) goto next_screen;
            if (!screen->device->busid) goto next_screen;
            if (!xconfigParsePciBusString(screen->device->busid,
                                          &bus0, &slot0, &scratch))
                goto next_screen;
            
            if ((bus0 == bus[i]) && (slot0 == slot[i])) {
                if (prev) {
                    prev->next = screen->next;
                } else {
                    config->screens = screen->next;
                }
                next = screen->next;
                screen->next = NULL;
                xconfigFreeScreenList(screen);
                screen = next;
            } else {
                
            next_screen:
                
                prev = screen;
                screen = screen->next;
            }
        }

        screenlist[i]->device->screen = -1;
    }
    
    /* wipe the existing adjacencies and recreate them */
    
    xconfigFreeAdjacencyList(layout->adjacencies);
    layout->adjacencies = NULL;
    
    create_adjacencies(op, config, layout);

    /* free unused device and monitor sections */
    
    free_unused_devices(op, config);
    free_unused_monitors(op, config);

    /* free stuff */

    free(screenlist);
    free(bus);
    free(slot);
    
    return TRUE;
    
} /* disable_separate_x_screens() */


/*
 * clone_display_list() - create a duplicate of the specified display
 * subsection.
 */

static XConfigDisplayPtr clone_display_list(XConfigDisplayPtr display0)
{
    XConfigDisplayPtr d = NULL, prev = NULL, head = NULL;
    
    while (display0) {
        d = nvalloc(sizeof(XConfigDisplayRec));
        memcpy(d, display0, sizeof(XConfigDisplayRec));
        if (display0->visual) d->visual = nvstrdup(display0->visual);
        if (display0->comment) d->comment = nvstrdup(display0->comment);
        d->options = xconfigOptionListDup(display0->options);
        d->next = NULL;
        if (prev) prev->next = d;
        if (!head) head = d;
        prev = d;
        display0 = display0->next;
    }
    
    return head;
    
} /* clone_display_list() */



/*
 * clone_device() - duplicate the specified device section, updating
 * the screen indices as approprate for multiple X screens on one GPU
 */

static XConfigDevicePtr clone_device(XConfigDevicePtr device0)
{
    XConfigDevicePtr device;

    device = nvalloc(sizeof(XConfigDeviceRec));
    
    device->identifier = nvstrcat(device0->identifier, " (2nd)", NULL);
    
    if (device0->vendor)  device->vendor  = nvstrdup(device0->vendor);
    if (device0->board)   device->board   = nvstrdup(device0->board);
    if (device0->chipset) device->chipset = nvstrdup(device0->chipset);
    if (device0->busid)   device->busid   = nvstrdup(device0->busid);
    if (device0->card)    device->card    = nvstrdup(device0->card);
    if (device0->driver)  device->driver  = nvstrdup(device0->driver);
    if (device0->ramdac)  device->ramdac  = nvstrdup(device0->ramdac);
    if (device0->comment) device->comment = nvstrdup(device0->comment);

    /* these are needed for multiple X screens on one GPU */

    device->screen = 1;
    device0->screen = 0;
          
    device->chipid = -1;
    device->chiprev = -1;
    device->irq = -1;

    device->options = xconfigOptionListDup(device0->options);
    
    /* insert the new device after the original device */
    
    device->next = device0->next;
    device0->next = device;

    return device;
    
} /* clone_device() */



/*
 * clone_screen() - duplicate the given screen, for use as the second
 * X screen on one GPU
 */

static XConfigScreenPtr clone_screen(XConfigScreenPtr screen0)
{
    XConfigScreenPtr screen = nvalloc(sizeof(XConfigScreenRec));
    
    screen->identifier = nvstrcat(screen0->identifier, " (2nd)", NULL);
    
    screen->device = clone_device(screen0->device);
    screen->device_name = nvstrdup(screen->device->identifier);
    
    screen->monitor = screen0->monitor;
    screen->monitor_name = nvstrdup(screen0->monitor_name);
    
    screen->defaultdepth = screen0->defaultdepth;
    
    screen->displays = clone_display_list(screen0->displays);
    
    screen->options = xconfigOptionListDup(screen0->options);
    if (screen0->comment) screen->comment = nvstrdup(screen0->comment);

    /* insert the new screen after the original screen */

    screen->next = screen0->next;
    screen0->next = screen;

    return screen;
    
} /* clone_screen() */



/*
 * create_adjacencies() - loop through all the screens in the config,
 * and add an adjacency section to the layout; this assumes that there
 * are no existing adjacencies in the layout
 */

static void create_adjacencies(Options *op, XConfigPtr config,
                               XConfigLayoutPtr layout)
{
    XConfigAdjacencyPtr adj, prev_adj;
    XConfigScreenPtr screen, prev;
    int i;
    
    prev = NULL;
    i = 0;
    prev_adj = NULL;
    
    for (screen = config->screens; screen; screen = screen->next) {
        
        adj = nvalloc(sizeof(XConfigAdjacencyRec));
        
        adj->scrnum = i;
        adj->screen_name = nvstrdup(screen->identifier);
        adj->screen = screen;
        
        if (prev_adj) {
            prev_adj->next = adj;
        } else {
            layout->adjacencies = adj;
        }
        
        prev_adj = adj;
        prev = screen;
        i++;
    }
    
    xconfigGenerateAssignScreenAdjacencies(layout);
    
} /* create_adjacencies() */


/*
 * enable_all_gpus() - get information for every GPU in the system,
 * and create a screen section for each
 *
 * XXX do we add new screens with reasonable defaults, or do we clone
 * the first existing X screen N times?  For now, we'll just add all
 * new X screens
 */

static int enable_all_gpus(Options *op, XConfigPtr config,
                           XConfigLayoutPtr layout)
{
    DevicesPtr pDevices;
    int i;

    pDevices = find_devices(op);
    if (!pDevices) {
        fmterr("Unable to determine number of GPUs in system; cannot "
               "honor '--enable-all-gpus' option.");
        return FALSE;
    }
    
    /* free all existing X screens, monitors, devices, and adjacencies */
    
    xconfigFreeScreenList(config->screens);
    config->screens = NULL;
    
    xconfigFreeDeviceList(config->devices);
    config->devices = NULL;
    
    xconfigFreeMonitorList(config->monitors);
    config->monitors = NULL;

    xconfigFreeAdjacencyList(layout->adjacencies);
    layout->adjacencies = NULL;

    /* add N new screens; this will also add device and monitor sections */
    
    for (i = 0; i < pDevices->nDevices; i++) {
        xconfigGenerateAddScreen(config,
                                 pDevices->devices[i].dev.bus,
                                 pDevices->devices[i].dev.slot,
                                 pDevices->devices[i].name, i);
    }
    
    free_devices(pDevices);

    /* create adjacencies for the layout */
    
    create_adjacencies(op, config, layout);

    return TRUE;
    
} /* enable_all_gpus() */



/*
 * free_unused_devices() - free unused device sections
 */

static void free_unused_devices(Options *op, XConfigPtr config)
{
    XConfigDevicePtr device, prev, next;
    XConfigScreenPtr screen;
    int found;

    /* free any unused device sections */
    
    device = config->devices;
    prev = NULL;
    
    while (device) {
        found = FALSE;
        
        for (screen = config->screens; screen; screen = screen->next) {
            if (device == screen->device) {
                found = TRUE;
                break;
            }
        }
        
        if (!found) {
            if (prev) {
                prev->next = device->next;
            } else {
                config->devices = device->next;
            }
            next = device->next;
            device->next = NULL;
            xconfigFreeDeviceList(device);
            device = next;
        } else {
            prev = device;
            device = device->next;
        }
    }
} /* free_unused_devices() */



/*
 * free_unused_monitors() - free unused monitor sections
 */

static void free_unused_monitors(Options *op, XConfigPtr config)
{
    XConfigMonitorPtr monitor, prev, next;
    XConfigScreenPtr screen;
    int found;

    /* free any unused monitor sections */
    
    monitor = config->monitors;
    prev = NULL;
    
    while (monitor) {
        found = FALSE;
        
        for (screen = config->screens; screen; screen = screen->next) {
            if (monitor == screen->monitor) {
                found = TRUE;
                break;
            }
        }
        
        if (!found) {
            if (prev) {
                prev->next = monitor->next;
            } else {
                config->monitors = monitor->next;
            }
            next = monitor->next;
            monitor->next = NULL;
            xconfigFreeMonitorList(monitor);
            monitor = next;
        } else {
            prev = monitor;
            monitor = monitor->next;
        }
    }
} /* free_unused_monitors() */



/*
 * only_one_screen() - delete all screens after the first one
 */

static int only_one_screen(Options *op, XConfigPtr config,
                           XConfigLayoutPtr layout)
{
    if (!config->screens) return FALSE;
    
    /* free all existing X screens after the first */
    
    xconfigFreeScreenList(config->screens->next);
    config->screens->next = NULL;
    
    /* free all adjacencies */
    
    xconfigFreeAdjacencyList(layout->adjacencies);
    layout->adjacencies = NULL;
    
    /* add new adjacency */
    
    create_adjacencies(op, config, layout);
    
    /* removed unused device and monitor sections */
    
    free_unused_devices(op, config);
    free_unused_monitors(op, config);

    return TRUE;

} /* only_one_screen() */

